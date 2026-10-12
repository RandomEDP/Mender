#include "SpectralEdit.h"
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
constexpr int edgeTaperBins = 2;

int validSize (int requested)
{
    int n = 256;
    while (n < requested && n < 16384)
        n <<= 1;
    return n;
}

// Hann analysis and synthesis; at 75 % overlap the squared windows sum to 1.5.
std::vector<double> hann (int n)
{
    std::vector<double> w ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        w[(std::size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * i / n);
    return w;
}
constexpr double synthesisScale = 1.0 / 1.5;

void analyse (const float* data, std::int64_t length, std::int64_t pos, const std::vector<double>& window,
              const Fft& fft, Spectrum& out)
{
    const auto n = (std::int64_t) window.size();
    for (std::int64_t i = 0; i < n; ++i)
    {
        const auto s = pos + i;
        out[(std::size_t) i] = (s >= 0 && s < length ? (double) data[s] : 0.0) * window[(std::size_t) i];
    }
    fft.forward (out);
}

// 1 inside [lowHz, highHz], tapering to 0 over a couple of bins outside it.
struct Band
{
    int lo = 0, hi = 0;
    std::vector<double> weight;
};

Band makeBand (int n, double sampleRate, double lowHz, double highHz)
{
    Band band;
    const int half = n / 2;
    const double binHz = sampleRate / n;
    const int boxLo = std::clamp ((int) std::floor (lowHz / binHz), 0, half);
    const int boxHi = std::clamp ((int) std::ceil (highHz / binHz), 0, half);
    band.lo = std::max (0, boxLo - edgeTaperBins);
    band.hi = std::min (half, boxHi + edgeTaperBins);
    band.weight.assign ((std::size_t) half + 1, 0.0);
    for (int b = band.lo; b <= band.hi; ++b)
    {
        const int outside = b < boxLo ? boxLo - b : (b > boxHi ? b - boxHi : 0);
        band.weight[(std::size_t) b] = 1.0 - (double) outside / (edgeTaperBins + 1);
    }
    return band;
}

// Frames for a paste: centres every hop through the region, first one a hop in.
struct PastePlan
{
    int n = 0, hop = 0;
    std::int64_t destStart = 0, destEnd = 0, firstFrame = 0;
    int numFrames = 0;
    bool valid = false;
};

PastePlan makePastePlan (std::int64_t numSamples, std::int64_t destStart, std::int64_t length, int fftSize)
{
    PastePlan p;
    p.n = validSize (fftSize);
    p.hop = p.n / 4;
    p.destStart = destStart;
    p.destEnd = destStart + length;
    if (length <= 0 || p.destEnd <= 0 || destStart >= numSamples)
        return p;

    p.firstFrame = destStart - p.n / 2 + p.hop;
    p.numFrames = (int) std::max<std::int64_t> (0, (p.destEnd - destStart - 1) / p.hop);
    p.valid = p.numFrames > 0;
    return p;
}
} // namespace

std::int64_t pasteContext (int fftSize)
{
    return validSize (fftSize);
}

EditRange pasteRange (std::int64_t numSamples, std::int64_t destStart, std::int64_t length, int fftSize)
{
    EditRange r;
    const auto plan = makePastePlan (numSamples, destStart, length, fftSize);
    if (! plan.valid)
        return r;

    r.start = std::max<std::int64_t> (0, plan.firstFrame);
    r.end = std::min<std::int64_t> (numSamples, plan.firstFrame + (std::int64_t) (plan.numFrames - 1) * plan.hop + plan.n);
    r.valid = r.end > r.start;
    return r;
}

EditRange pasteBand (float* const* dest, int numChannels, std::int64_t numSamples, double sampleRate,
                     const float* const* source, int sourceChannels, std::int64_t sourceLength, std::int64_t context,
                     std::int64_t destStart, std::int64_t length, double lowHz, double highHz, int fftSize)
{
    const auto plan = makePastePlan (numSamples, destStart, length, fftSize);
    const auto range = pasteRange (numSamples, destStart, length, fftSize);
    if (! plan.valid || ! range.valid || sourceChannels <= 0 || highHz <= lowHz || sampleRate <= 0.0)
        return {};

    const int n = plan.n, half = n / 2;
    const auto window = hann (n);
    const auto band = makeBand (n, sampleRate, lowHz, highHz);
    const Fft fft ((std::size_t) n);
    Spectrum target ((std::size_t) n), copied ((std::size_t) n), delta ((std::size_t) n);
    std::vector<double> correction;
    const double fade = 0.5 * n;

    for (int c = 0; c < numChannels; ++c)
    {
        const float* src = source[std::min (c, sourceChannels - 1)];
        correction.assign ((std::size_t) (range.end - range.start), 0.0);

        for (int k = 0; k < plan.numFrames; ++k)
        {
            const auto pos = plan.firstFrame + (std::int64_t) k * plan.hop;
            const double centre = (double) pos + 0.5 * n;

            // Fade the paste in and out over half a frame at each end.
            const double timeWeight = std::clamp ((centre - (double) plan.destStart) / fade, 0.0, 1.0)
                                      * std::clamp (((double) plan.destEnd - centre) / fade, 0.0, 1.0);
            if (timeWeight <= 0.0)
                continue;

            analyse (dest[c], numSamples, pos, window, fft, target);
            analyse (src, sourceLength, pos - destStart + context, window, fft, copied);

            std::fill (delta.begin(), delta.end(), std::complex<double>());
            for (int b = band.lo; b <= band.hi; ++b)
            {
                const auto ub = (std::size_t) b;
                const auto d = (copied[ub] - target[ub]) * (timeWeight * band.weight[ub]);
                delta[ub] = d;
                if (b > 0 && b < half)
                    delta[(std::size_t) (n - b)] = std::conj (d);
            }

            fft.inverse (delta);
            for (int i = 0; i < n; ++i)
            {
                const auto s = pos + i;
                if (s >= range.start && s < range.end)
                    correction[(std::size_t) (s - range.start)] += delta[(std::size_t) i].real() * window[(std::size_t) i] * synthesisScale;
            }
        }

        for (std::size_t i = 0; i < correction.size(); ++i)
            dest[c][range.start + (std::int64_t) i] += (float) correction[i];
    }

    return range;
}

void bandPass (const float* in, float* out, std::int64_t length, double sampleRate,
               double lowHz, double highHz, int fftSize)
{
    std::fill (out, out + length, 0.0f);
    if (length <= 0 || sampleRate <= 0.0 || highHz <= lowHz)
        return;

    const int n = validSize (fftSize), hop = n / 4, half = n / 2;
    const auto window = hann (n);
    const auto band = makeBand (n, sampleRate, lowHz, highHz);
    const Fft fft ((std::size_t) n);
    Spectrum spec ((std::size_t) n), kept ((std::size_t) n);
    std::vector<double> sum ((std::size_t) length, 0.0);

    for (std::int64_t pos = -n + hop; pos < length; pos += hop)
    {
        analyse (in, length, pos, window, fft, spec);
        std::fill (kept.begin(), kept.end(), std::complex<double>());
        for (int b = band.lo; b <= band.hi; ++b)
        {
            const auto ub = (std::size_t) b;
            kept[ub] = spec[ub] * band.weight[ub];
            if (b > 0 && b < half)
                kept[(std::size_t) (n - b)] = std::conj (kept[ub]);
        }

        fft.inverse (kept);
        for (int i = 0; i < n; ++i)
        {
            const auto s = pos + i;
            if (s >= 0 && s < length)
                sum[(std::size_t) s] += kept[(std::size_t) i].real() * window[(std::size_t) i] * synthesisScale;
        }
    }

    for (std::int64_t i = 0; i < length; ++i)
        out[i] = (float) sum[(std::size_t) i];
}
} // namespace mender
