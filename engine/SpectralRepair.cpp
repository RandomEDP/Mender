#include "SpectralRepair.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace mender
{
namespace
{
using Spectrum = std::vector<std::complex<double>>;

constexpr double pi = 3.14159265358979323846;
constexpr double tiny = 1.0e-12;
constexpr int edgeTaperBins = 2;

int validFftSize (int requested)
{
    int n = 256;
    while (n < requested && n < 16384)
        n <<= 1;
    return n;
}

struct Plan
{
    int n = 0, hop = 0;
    std::int64_t selStart = 0, selEnd = 0;
    std::int64_t firstFrame = 0; // position of the first frame overlapping the box
    int numFrames = 0;
    bool valid = false;
};

Plan makePlan (std::int64_t numSamples, const Selection& sel, const Settings& settings)
{
    Plan p;
    p.n = validFftSize (settings.fftSize);
    p.hop = p.n / 4;
    p.selStart = std::clamp<std::int64_t> (sel.startSample, 0, numSamples);
    p.selEnd = std::clamp<std::int64_t> (sel.endSample, 0, numSamples);

    if (p.selEnd <= p.selStart)
        return p;

    // Frames sit on a grid anchored so that one frame ends exactly at the
    // start of the box; the next one is the first to overlap it.
    p.firstFrame = p.selStart - p.n + p.hop;
    p.numFrames = (int) ((p.selEnd - 1 - p.firstFrame) / p.hop) + 1;
    p.valid = true;
    return p;
}

struct Channel
{
    const float* data;
    std::int64_t length;

    double at (std::int64_t i) const { return (i >= 0 && i < length) ? (double) data[i] : 0.0; }
};

void analyse (const Channel& ch, std::int64_t pos, const std::vector<double>& window,
              const Fft& fft, Spectrum& out)
{
    const auto n = (std::int64_t) window.size();
    for (std::int64_t i = 0; i < n; ++i)
        out[(std::size_t) i] = ch.at (pos + i) * window[(std::size_t) i];
    fft.forward (out);
}

// Median magnitude per bin over a run of frames that lie fully inside the
// audio. The median ignores a note starting or stopping in a few of the frames.
// Returns false if no such frame exists.
bool referenceMagnitudes (const Channel& ch, std::int64_t firstPos, int step, int count,
                          const std::vector<double>& window, const Fft& fft,
                          int binLo, int binHi, std::vector<double>& mags)
{
    const auto n = (std::int64_t) window.size();
    const auto width = (std::size_t) (binHi - binLo + 1);
    Spectrum spec ((std::size_t) n);
    std::vector<double> frames; // one row of bins per usable frame
    std::size_t used = 0;

    for (int j = 0; j < count; ++j)
    {
        const auto pos = firstPos + (std::int64_t) j * step;
        if (pos < 0 || pos + n > ch.length)
            break;

        analyse (ch, pos, window, fft, spec);
        for (int b = binLo; b <= binHi; ++b)
            frames.push_back (std::abs (spec[(std::size_t) b]));
        ++used;
    }

    if (used == 0)
        return false;

    std::vector<double> column (used);
    for (std::size_t b = 0; b < width; ++b)
    {
        for (std::size_t f = 0; f < used; ++f)
            column[f] = frames[f * width + b];

        const auto middle = column.begin() + (std::ptrdiff_t) (used / 2);
        std::nth_element (column.begin(), middle, column.end());
        mags[(std::size_t) binLo + b] = *middle;
    }

    return true;
}

double logInterp (double a, double b, double t)
{
    return std::exp ((1.0 - t) * std::log (a + tiny) + t * std::log (b + tiny));
}

// Mean magnitude of a few bins on one side of the box, within one frame.
bool sideLevel (const Spectrum& spec, int from, int to, int half, double& level)
{
    from = std::max (from, 1);
    to = std::min (to, half - 1);
    if (to < from)
        return false;

    double sum = 0.0;
    for (int b = from; b <= to; ++b)
        sum += std::abs (spec[(std::size_t) b]);
    level = sum / (to - from + 1);
    return true;
}
} // namespace

Result affectedRange (std::int64_t numSamples, const Selection& sel, const Settings& settings)
{
    Result r;
    const auto plan = makePlan (numSamples, sel, settings);
    if (! plan.valid)
        return r;

    r.changed = true;
    r.changedStart = std::max<std::int64_t> (0, plan.firstFrame);
    r.changedEnd = std::min<std::int64_t> (numSamples,
                                           plan.firstFrame + (std::int64_t) (plan.numFrames - 1) * plan.hop + plan.n);
    return r;
}

Result repair (float* const* channels, int numChannels, std::int64_t numSamples,
               double sampleRate, const Selection& sel, const Settings& settings)
{
    Result result;
    const auto plan = makePlan (numSamples, sel, settings);
    const double strength = std::clamp (settings.strength, 0.0, 1.0);

    if (! plan.valid || sampleRate <= 0.0 || strength <= 0.0 || sel.highHz <= sel.lowHz)
        return result;

    const int n = plan.n, hop = plan.hop, half = n / 2;
    const double binHz = sampleRate / n;
    const int boxLo = std::clamp ((int) std::floor (sel.lowHz / binHz), 0, half);
    const int boxHi = std::clamp ((int) std::ceil (sel.highHz / binHz), 0, half);
    const int binLo = std::max (0, boxLo - edgeTaperBins);
    const int binHi = std::min (half, boxHi + edgeTaperBins);
    const double allowed = std::pow (10.0, settings.thresholdDb / 20.0);

    // Hann analysis and synthesis windows (low leakage, so a loud partial does
    // not hide what sits beside it). At 75 % overlap their squares sum to
    // exactly 1.5, so dividing by 1.5 gives unity gain.
    std::vector<double> window ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * i / n);
    const double synthesisScale = 1.0 / 1.5;

    // Weight per bin: 1 inside the box, tapering to 0 just outside it.
    std::vector<double> binWeight ((std::size_t) half + 1, 0.0);
    for (int b = binLo; b <= binHi; ++b)
    {
        const int outside = b < boxLo ? boxLo - b : (b > boxHi ? b - boxHi : 0);
        binWeight[(std::size_t) b] = 1.0 - (double) outside / (edgeTaperBins + 1);
    }

    const Fft fft ((std::size_t) n);
    Spectrum spec ((std::size_t) n), delta ((std::size_t) n);
    std::vector<double> before ((std::size_t) half + 1), after ((std::size_t) half + 1);
    std::vector<double> correction;

    const auto range = affectedRange (numSamples, sel, settings);
    const int refFrames = std::max (1, settings.referenceFrames);
    const std::int64_t lastFrame = plan.firstFrame + (std::int64_t) (plan.numFrames - 1) * hop;
    const int sideBins = std::max (3, (boxHi - boxLo) / 4);

    for (int c = 0; c < numChannels; ++c)
    {
        const Channel ch { channels[c], numSamples };

        bool haveBefore = false, haveAfter = false;
        if (settings.reference == Reference::time)
        {
            // Walk outwards from the box on each side.
            haveBefore = referenceMagnitudes (ch, plan.firstFrame - hop, -hop, refFrames,
                                            window, fft, binLo, binHi, before);
            haveAfter = referenceMagnitudes (ch, lastFrame + hop, hop, refFrames,
                                           window, fft, binLo, binHi, after);
        }
        const bool useTime = haveBefore || haveAfter; // otherwise fall back to frequency

        correction.assign ((std::size_t) (range.changedEnd - range.changedStart), 0.0);

        for (int k = 0; k < plan.numFrames; ++k)
        {
            const auto pos = plan.firstFrame + (std::int64_t) k * hop;
            analyse (ch, pos, window, fft, spec);

            const double centre = (double) pos + 0.5 * n;
            const double u = std::clamp ((centre - (double) plan.selStart)
                                             / (double) (plan.selEnd - plan.selStart), 0.0, 1.0);

            double below = 0.0, above = 0.0;
            bool haveBelow = false, haveAbove = false;
            if (! useTime)
            {
                haveBelow = sideLevel (spec, binLo - sideBins, binLo - 1, half, below);
                haveAbove = sideLevel (spec, binHi + 1, binHi + sideBins, half, above);
                if (! haveBelow && ! haveAbove)
                    continue; // box covers the whole spectrum and there is nothing to compare with
            }

            std::fill (delta.begin(), delta.end(), std::complex<double>());
            bool any = false;

            for (int b = binLo; b <= binHi; ++b)
            {
                const auto ub = (std::size_t) b;
                double target;

                if (useTime)
                {
                    target = haveBefore && haveAfter ? logInterp (before[ub], after[ub], u)
                                                     : (haveBefore ? before[ub] : after[ub]);
                }
                else
                {
                    const double v = binHi > binLo ? (double) (b - binLo) / (binHi - binLo) : 0.5;
                    target = haveBelow && haveAbove ? logInterp (below, above, v)
                                                    : (haveBelow ? below : above);
                }

                const double mag = std::abs (spec[ub]);
                const double limit = target * allowed;
                if (mag <= limit || mag <= tiny)
                    continue;

                const double gain = 1.0 + strength * binWeight[ub] * (limit / mag - 1.0);
                const auto d = spec[ub] * (gain - 1.0);
                delta[ub] = d;
                if (b > 0 && b < half)
                    delta[(std::size_t) (n - b)] = std::conj (d);
                any = true;
            }

            if (! any)
                continue;

            fft.inverse (delta);
            for (int i = 0; i < n; ++i)
            {
                const auto s = pos + i;
                if (s >= range.changedStart && s < range.changedEnd)
                    correction[(std::size_t) (s - range.changedStart)]
                        += delta[(std::size_t) i].real() * window[(std::size_t) i] * synthesisScale;
            }
            result.changed = true;
        }

        for (std::size_t i = 0; i < correction.size(); ++i)
            channels[c][range.changedStart + (std::int64_t) i] += (float) correction[i];
    }

    result.changedStart = range.changedStart;
    result.changedEnd = range.changedEnd;
    return result;
}
} // namespace mender
