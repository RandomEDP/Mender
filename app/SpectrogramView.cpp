#include "SpectrogramView.h"

#include "Fft.h"

namespace
{
constexpr int overviewRows = 768;
constexpr int leftGutter = 58, bottomGutter = 22, topGutter = 16, rightGutter = 10;
constexpr double minFrequencyRatio = 1.25; // narrowest view: about a third of an octave
constexpr float edgeGrab = 5.0f, capGrab = 7.0f;
constexpr int detailDelayMs = 70;          // redraw sharply once the view has been still this long

juce::String hzLabel (double hz)
{
    if (hz >= 1000.0)
    {
        const double k = hz / 1000.0;
        return juce::String (k, std::abs (k - std::round (k)) < 0.01 ? 0 : 1) + "k";
    }
    return juce::String (juce::roundToInt (hz));
}

juce::String timeLabel (double seconds, double step)
{
    const int decimals = step >= 1.0 ? 0 : (step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3));
    const int minutes = (int) (seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    return juce::String (minutes) + ":" + juce::String (rest, decimals).paddedLeft ('0', decimals == 0 ? 2 : decimals + 3);
}

std::vector<double> hannWindow (int n)
{
    std::vector<double> w ((size_t) n);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n);
    return w;
}

// Which FFT bins feed each picture row. Rows are log-spaced, row 0 at the top.
struct RowMap
{
    RowMap (int rows, double topHz, double bottomHz, double binHz)
        : low ((size_t) rows), high ((size_t) rows)
    {
        const auto edge = [=] (double r) { return topHz * std::pow (bottomHz / topHz, r / rows); };
        for (int r = 0; r < rows; ++r)
        {
            high[(size_t) r] = edge (r) / binHz;
            low[(size_t) r] = edge (r + 1) / binHz;
        }
    }

    std::vector<double> low, high;
};

// Turns one windowed frame (already in `spec`) into a level in dB per row.
void framesToRows (std::vector<std::complex<double>>& spec, const mender::Fft& fft, std::vector<double>& mags,
                   const RowMap& map, float* out)
{
    const int n = (int) spec.size(), half = n / 2;
    fft.forward (spec);

    const double scale = 4.0 / n; // full-scale sine reads 0 dB
    for (int b = 0; b <= half; ++b)
        mags[(size_t) b] = std::abs (spec[(size_t) b]) * scale;

    for (size_t r = 0; r < map.low.size(); ++r)
    {
        const double lo = map.low[r], hi = map.high[r];
        double m;

        if (hi - lo < 1.0)
        {
            // Several rows per bin: interpolate.
            const double pos = juce::jlimit (0.0, (double) half, 0.5 * (lo + hi));
            const int b = juce::jmin ((int) pos, half - 1);
            const double frac = pos - b;
            m = mags[(size_t) b] * (1.0 - frac) + mags[(size_t) b + 1] * frac;
        }
        else
        {
            // Several bins per row: show the loudest.
            m = 0.0;
            const int from = juce::jlimit (0, half, (int) lo), to = juce::jlimit (0, half, (int) std::ceil (hi));
            for (int b = from; b <= to; ++b)
                m = juce::jmax (m, mags[(size_t) b]);
        }

        out[r] = (float) (20.0 * std::log10 (m + 1.0e-12));
    }
}
} // namespace

//==============================================================================
struct SpectrogramView::Detail
{
    juce::Image image;
    std::vector<float> levels; // width * height, column by column
    double t0 = 0.0, t1 = 0.0, f0 = 0.0, f1 = 0.0;
    int width = 0, height = 0, fftSize = 0;
    std::uint64_t generation = 0;
};

// Draws the visible part of the spectrogram at screen resolution, off the message thread.
class SpectrogramView::Renderer final : public juce::Thread
{
public:
    struct Job
    {
        const float* data = nullptr;
        juce::int64 length = 0;
        double sampleRate = 0.0, t0 = 0.0, t1 = 0.0, f0 = 0.0, f1 = 0.0;
        int width = 0, height = 0, fftSize = 2048;
        std::uint64_t generation = 0;
    };

    explicit Renderer (SpectrogramView& v)
        : juce::Thread ("Mender spectrogram"), owner (v), safeOwner (&v)
    {
        startThread (Priority::low);
    }

    ~Renderer() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    void submit (const Job& job)
    {
        {
            const juce::ScopedLock guard (lock);
            pending = job;
            hasPending = true;
        }
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            Job job;
            bool have = false;
            {
                const juce::ScopedLock guard (lock);
                if (hasPending)
                {
                    job = pending;
                    hasPending = false;
                    have = true;
                }
            }

            if (! have)
            {
                wait (-1);
                continue;
            }

            if (auto result = render (job))
                juce::MessageManager::callAsync ([target = safeOwner, result]
                {
                    if (auto* view = target.getComponent())
                        view->detailReady (result);
                });
        }
    }

private:
    bool cancelled (const Job& job) const
    {
        return threadShouldExit() || owner.generation.load() != job.generation;
    }

    std::shared_ptr<Detail> render (const Job& job)
    {
        const int n = job.fftSize, half = n / 2, width = job.width, height = job.height;
        const double visible = job.t1 - job.t0;
        const double columnStep = visible / width;

        // Frames no closer than an eighth of their length; columns between them are interpolated.
        const double frameStep = juce::jmax (columnStep, n / 8.0);
        const double firstCentre = job.t0 - frameStep;
        const int numFrames = (int) std::ceil ((visible + 2.0 * frameStep) / frameStep) + 1;

        const auto window = hannWindow (n);
        const mender::Fft fft ((size_t) n);
        const RowMap map (height, job.f1, job.f0, job.sampleRate / n);
        std::vector<std::complex<double>> spec ((size_t) n);
        std::vector<double> mags ((size_t) half + 1);
        std::vector<float> frameLevels ((size_t) numFrames * (size_t) height);

        for (int k = 0; k < numFrames; ++k)
        {
            const auto start = (juce::int64) std::llround (firstCentre + k * frameStep) - half;
            {
                const juce::ScopedReadLock guard (owner.audioLock);
                if (cancelled (job))
                    return {};

                for (int i = 0; i < n; ++i)
                {
                    const auto s = start + i;
                    spec[(size_t) i] = (s >= 0 && s < job.length ? (double) job.data[s] : 0.0) * window[(size_t) i];
                }
            }

            framesToRows (spec, fft, mags, map, frameLevels.data() + (size_t) k * (size_t) height);
        }

        auto result = std::make_shared<Detail>();
        result->levels.resize ((size_t) width * (size_t) height);

        for (int x = 0; x < width; ++x)
        {
            const double pos = (job.t0 + (x + 0.5) * columnStep - firstCentre) / frameStep;
            const int k = juce::jlimit (0, numFrames - 2, (int) std::floor (pos));
            const float frac = (float) juce::jlimit (0.0, 1.0, pos - k);
            const float* a = frameLevels.data() + (size_t) k * (size_t) height;
            const float* b = a + height;
            float* out = result->levels.data() + (size_t) x * (size_t) height;

            for (int r = 0; r < height; ++r)
                out[r] = a[r] + (b[r] - a[r]) * frac;
        }

        if (cancelled (job))
            return {};

        result->image = juce::Image (juce::Image::ARGB, width, height, false, juce::SoftwareImageType());
        result->t0 = job.t0;
        result->t1 = job.t1;
        result->f0 = job.f0;
        result->f1 = job.f1;
        result->width = width;
        result->height = height;
        result->fftSize = n;
        result->generation = job.generation;
        return result;
    }

    SpectrogramView& owner;
    juce::Component::SafePointer<SpectrogramView> safeOwner;
    juce::CriticalSection lock;
    Job pending;
    bool hasPending = false;
};

//==============================================================================
SpectrogramView::SpectrogramView()
{
    // Dark-to-bright heat map: quiet is black/violet, loud is orange/pale yellow.
    const juce::Colour stops[] = { juce::Colour (0, 0, 4), juce::Colour (40, 11, 84), juce::Colour (101, 21, 110),
                                   juce::Colour (159, 42, 99), juce::Colour (212, 72, 66), juce::Colour (245, 125, 21),
                                   juce::Colour (250, 193, 39), juce::Colour (252, 255, 164) };
    const int numStops = (int) std::size (stops);

    for (int i = 0; i < 256; ++i)
    {
        const double pos = i / 255.0 * (numStops - 1);
        const int a = juce::jmin ((int) pos, numStops - 2);
        palette[(size_t) i] = stops[a].interpolatedWith (stops[a + 1], (float) (pos - a)).getPixelARGB();
    }

    setOpaque (true);
    renderer = std::make_unique<Renderer> (*this);
}

SpectrogramView::~SpectrogramView()
{
    stopTimer();
    ++generation;
    renderer.reset();
}

//==============================================================================
void SpectrogramView::setAudio (const juce::AudioBuffer<float>* buffer, double rate)
{
    ++generation;
    {
        const juce::ScopedWriteLock guard (audioLock);
        audio = buffer;
        sampleRate = rate;
        if (audio == nullptr || displayChannel >= audio->getNumChannels())
            displayChannel = -1;

        detail.reset();
        staleRange = {};
        rebuildImage();
    }

    selectionActive = false;
    cursor = 0;
    playhead = -1;
    zoomToFit();

    if (onSelectionChanged)
        onSelectionChanged();
}

void SpectrogramView::setDisplayChannel (int channel)
{
    if (audio != nullptr && channel >= audio->getNumChannels())
        channel = -1;

    if (channel == displayChannel)
        return;

    ++generation;
    {
        const juce::ScopedWriteLock guard (audioLock);
        displayChannel = channel;
        detail.reset();
        staleRange = {};
        rebuildImage();
    }

    repaint();
    startTimer (detailDelayMs);
}

void SpectrogramView::setDisplayRange (double newFloorDb, double newCeilingDb)
{
    floorDb = newFloorDb;
    ceilingDb = juce::jmax (newFloorDb + 6.0, newCeilingDb);

    if (image.isValid())
        colour (image, levels, overviewRows, 0, image.getWidth() - 1);
    if (detail != nullptr)
        colour (detail->image, detail->levels, detail->height, 0, detail->width - 1);

    repaint();
}

const float* SpectrogramView::displayData() const
{
    if (audio == nullptr)
        return nullptr;
    if (displayChannel >= 0)
        return audio->getReadPointer (displayChannel);
    return audio->getNumChannels() == 1 ? audio->getReadPointer (0) : mix.data();
}

void SpectrogramView::updateMix (int firstSample, int lastSample)
{
    if (audio == nullptr || displayChannel >= 0 || audio->getNumChannels() < 2 || mix.empty())
        return;

    const int channels = audio->getNumChannels();
    firstSample = juce::jmax (0, firstSample);
    lastSample = juce::jmin (audio->getNumSamples(), lastSample);
    if (lastSample <= firstSample)
        return;

    const int count = lastSample - firstSample;
    float* out = mix.data() + firstSample;
    juce::FloatVectorOperations::copy (out, audio->getReadPointer (0, firstSample), count);
    for (int c = 1; c < channels; ++c)
        juce::FloatVectorOperations::add (out, audio->getReadPointer (c, firstSample), count);
    juce::FloatVectorOperations::multiply (out, 1.0f / (float) channels, count);
}

void SpectrogramView::rebuildImage()
{
    image = {};
    levels.clear();
    mix.clear();

    if (audio == nullptr || audio->getNumSamples() == 0 || sampleRate <= 0.0)
        return;

    fftSize = sampleRate > 100000.0 ? 8192 : (sampleRate > 50000.0 ? 4096 : 2048);
    // Keep the overview to a few thousand columns however long the clip is.
    hop = juce::jmax (fftSize / 8, (int) std::ceil (audio->getNumSamples() / 6000.0));
    maxHz = sampleRate * 0.5;
    minHz = 20.0;

    if (displayChannel < 0 && audio->getNumChannels() > 1)
    {
        mix.resize ((size_t) audio->getNumSamples());
        updateMix (0, audio->getNumSamples());
    }

    const int columns = audio->getNumSamples() / hop + 1;
    image = juce::Image (juce::Image::ARGB, columns, overviewRows, true, juce::SoftwareImageType());
    levels.assign ((size_t) columns * overviewRows, -200.0f);
    renderColumns (0, columns - 1);
}

void SpectrogramView::renderColumns (int firstColumn, int lastColumn)
{
    const float* data = displayData();
    if (! image.isValid() || data == nullptr)
        return;

    firstColumn = juce::jmax (0, firstColumn);
    lastColumn = juce::jmin (image.getWidth() - 1, lastColumn);
    if (lastColumn < firstColumn)
        return;

    const int numSamples = audio->getNumSamples();
    const auto window = hannWindow (fftSize);
    const RowMap map (overviewRows, maxHz, minHz, sampleRate / fftSize);
    const mender::Fft fft ((size_t) fftSize);
    std::vector<std::complex<double>> spec ((size_t) fftSize);
    std::vector<double> mags ((size_t) fftSize / 2 + 1);

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        const int start = column * hop - fftSize / 2;
        for (int i = 0; i < fftSize; ++i)
        {
            const int s = start + i;
            spec[(size_t) i] = (s >= 0 && s < numSamples ? (double) data[s] : 0.0) * window[(size_t) i];
        }

        framesToRows (spec, fft, mags, map, levels.data() + (size_t) column * overviewRows);
    }

    colour (image, levels, overviewRows, firstColumn, lastColumn);
}

void SpectrogramView::colour (juce::Image& target, const std::vector<float>& source, int rows,
                              int firstColumn, int lastColumn) const
{
    if (! target.isValid())
        return;

    firstColumn = juce::jmax (0, firstColumn);
    lastColumn = juce::jmin (target.getWidth() - 1, lastColumn);
    if (lastColumn < firstColumn)
        return;

    const float offset = (float) floorDb;
    const float factor = (float) (255.0 / (ceilingDb - floorDb));
    juce::Image::BitmapData pixels (target, firstColumn, 0, lastColumn - firstColumn + 1, rows,
                                    juce::Image::BitmapData::writeOnly);

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        const float* level = source.data() + (size_t) column * (size_t) rows;
        for (int r = 0; r < rows; ++r)
        {
            const int index = juce::jlimit (0, 255, (int) ((level[r] - offset) * factor + 0.5f));
            ((juce::PixelARGB*) pixels.getPixelPointer (column - firstColumn, r))->set (palette[(size_t) index]);
        }
    }
}

void SpectrogramView::audioChanged (juce::int64 startSample, juce::int64 endSample)
{
    ++generation;
    {
        const juce::ScopedWriteLock guard (audioLock);
        updateMix ((int) startSample, (int) endSample);
    }

    renderColumns ((int) ((startSample - fftSize) / hop), (int) ((endSample + fftSize) / hop) + 1);

    const juce::Range<juce::int64> changed (startSample, endSample);
    staleRange = staleRange.isEmpty() ? changed : staleRange.getUnionWith (changed);
    repaint();
    startTimer (detailDelayMs);
}

//==============================================================================
void SpectrogramView::timerCallback()
{
    stopTimer();
    requestDetail();
}

int SpectrogramView::detailFftSize (int heightPixels) const
{
    // Frequency: fine enough that neighbouring rows in the middle of the view differ.
    const double midHz = std::sqrt (viewLowHz * viewHighHz);
    const double rowHz = midHz * (std::pow (viewHighHz / viewLowHz, 1.0 / juce::jmax (1, heightPixels)) - 1.0);
    int n = 256;
    while (n < 32768 && sampleRate / n > 2.0 * rowHz)
        n <<= 1;
    n = juce::jlimit (fftSize / 2, juce::jmin (32768, fftSize * 8), n);

    // Time: when zoomed in close, shorter frames keep fast sounds sharp.
    while (n > 256 && n > (viewEnd - viewStart) / 3.0)
        n >>= 1;
    return n;
}

void SpectrogramView::requestDetail()
{
    const float* data = displayData();
    const auto plot = plotArea();
    if (data == nullptr || ! image.isValid() || plot.isEmpty())
        return;

    const double scale = juce::jlimit (1.0, 3.0, (double) juce::Component::getApproximateScaleFactorForComponent (this));

    Renderer::Job job;
    job.data = data;
    job.length = audio->getNumSamples();
    job.sampleRate = sampleRate;
    job.t0 = viewStart;
    job.t1 = viewEnd;
    job.f0 = viewLowHz;
    job.f1 = viewHighHz;
    job.width = juce::jlimit (16, 4096, juce::roundToInt (plot.getWidth() * scale));
    job.height = juce::jlimit (16, 2048, juce::roundToInt (plot.getHeight() * scale));
    job.fftSize = detailFftSize (job.height);
    job.generation = ++generation;
    renderer->submit (job);
}

void SpectrogramView::detailReady (std::shared_ptr<Detail> result)
{
    if (result == nullptr || result->generation != generation.load())
        return; // something changed since it was asked for; a newer one is on its way

    colour (result->image, result->levels, result->height, 0, result->width - 1);
    detail = std::move (result);
    staleRange = {};
    repaint();
}

bool SpectrogramView::isDetailCurrent() const
{
    return detail != nullptr && ! isTimerRunning() && detail->generation == generation.load()
           && juce::exactlyEqual (detail->t0, viewStart) && juce::exactlyEqual (detail->t1, viewEnd)
           && juce::exactlyEqual (detail->f0, viewLowHz) && juce::exactlyEqual (detail->f1, viewHighHz);
}

//==============================================================================
void SpectrogramView::setSelection (const mender::Selection& s)
{
    selection = s;
    selectionActive = s.endSample > s.startSample && s.highHz > s.lowHz;
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

void SpectrogramView::clearSelection()
{
    setSelection ({});
}

void SpectrogramView::setFullBand (bool shouldBeFullBand)
{
    fullBand = shouldBeFullBand;

    if (fullBand && selectionActive && sampleRate > 0.0)
    {
        auto s = selection;
        s.lowHz = 0.0;
        s.highHz = maxHz;
        setSelection (s);
    }

    repaint();
}

void SpectrogramView::setReferenceMode (mender::Reference mode)
{
    referenceMode = mode;
    repaint();
}

void SpectrogramView::setReach (const Reach& r)
{
    reach = r;
    repaint();
}

void SpectrogramView::setMinimumReach (double seconds)
{
    minimumReach = seconds;
    repaint();
}

SpectrogramView::Reach SpectrogramView::getEffectiveReach() const
{
    auto r = reach;
    r.beforeSeconds = juce::jmax (r.beforeSeconds, minimumReach);
    r.afterSeconds = juce::jmax (r.afterSeconds, minimumReach);
    return r;
}

void SpectrogramView::setCursor (juce::int64 sample)
{
    const auto total = audio != nullptr ? (juce::int64) audio->getNumSamples() : 0;
    sample = juce::jlimit<juce::int64> (0, total, sample);
    if (sample != cursor)
    {
        cursor = sample;
        repaint();
    }
}

void SpectrogramView::setPlayhead (juce::int64 sample)
{
    if (sample != playhead)
    {
        playhead = sample;
        repaint();
    }
}

void SpectrogramView::setView (double start, double end)
{
    const double total = audio != nullptr ? (double) audio->getNumSamples() : 1.0;
    const double minLength = juce::jmin (total, 64.0);
    const double length = juce::jlimit (minLength, juce::jmax (minLength, total), end - start);
    start = juce::jlimit (0.0, juce::jmax (0.0, total - length), start);

    if (juce::exactlyEqual (start, viewStart) && juce::exactlyEqual (start + length, viewEnd))
        return;

    viewStart = start;
    viewEnd = start + length;
    repaint();
    startTimer (detailDelayMs);
}

void SpectrogramView::setFrequencyView (double lowHz, double highHz)
{
    // Work in log frequency so zooming feels even across the range.
    const double fullSpan = std::log (maxHz / minHz);
    const double span = juce::jlimit (std::log (minFrequencyRatio), fullSpan,
                                      std::log (juce::jmax (highHz, 1.0) / juce::jmax (lowHz, 1.0)));
    const double low = juce::jlimit (std::log (minHz), std::log (maxHz) - span, std::log (juce::jmax (lowHz, 1.0)));
    const double newLow = std::exp (low), newHigh = std::exp (low + span);

    if (juce::exactlyEqual (newLow, viewLowHz) && juce::exactlyEqual (newHigh, viewHighHz))
        return;

    viewLowHz = newLow;
    viewHighHz = newHigh;
    repaint();
    startTimer (detailDelayMs);
}

void SpectrogramView::zoomToFit()
{
    setView (0.0, audio != nullptr ? (double) audio->getNumSamples() : 1.0);
    setFrequencyView (minHz, maxHz);
}

void SpectrogramView::zoomToSelection()
{
    if (! selectionActive)
        return;

    const auto r = getEffectiveReach();
    const bool timeReach = showsHandle (Handle::reachBefore);
    const double before = timeReach ? r.beforeSeconds * sampleRate : 0.0;
    const double after = timeReach ? r.afterSeconds * sampleRate : 0.0;
    const double length = (double) (selection.endSample - selection.startSample);
    setView ((double) selection.startSample - before - length * 0.5, (double) selection.endSample + after + length * 0.5);

    if (fullBand || selection.lowHz <= minHz)
        setFrequencyView (minHz, fullBand ? maxHz : selection.highHz * 1.5);
    else
        setFrequencyView (selection.lowHz / 1.5, selection.highHz * 1.5);
}

void SpectrogramView::zoomTime (double factor)
{
    const double length = viewEnd - viewStart;
    const double anchor = ((double) cursor >= viewStart && (double) cursor <= viewEnd) ? (double) cursor : viewStart + 0.5 * length;
    const double proportion = (anchor - viewStart) / length;
    const double newLength = length * factor;
    setView (anchor - proportion * newLength, anchor + (1.0 - proportion) * newLength);
}

void SpectrogramView::zoomFrequency (double factor)
{
    const double centre = std::sqrt (viewLowHz * viewHighHz);
    const double halfSpan = 0.5 * std::log (viewHighHz / viewLowHz) * factor;
    setFrequencyView (centre * std::exp (-halfSpan), centre * std::exp (halfSpan));
}

//==============================================================================
juce::Rectangle<int> SpectrogramView::plotArea() const
{
    return getLocalBounds().withTrimmedLeft (leftGutter).withTrimmedBottom (bottomGutter)
                           .withTrimmedTop (topGutter).withTrimmedRight (rightGutter);
}

double SpectrogramView::xToSample (float x) const
{
    const auto plot = plotArea();
    return viewStart + (x - (float) plot.getX()) / (double) juce::jmax (1, plot.getWidth()) * (viewEnd - viewStart);
}

float SpectrogramView::sampleToX (double sample) const
{
    const auto plot = plotArea();
    return (float) (plot.getX() + (sample - viewStart) / (viewEnd - viewStart) * plot.getWidth());
}

double SpectrogramView::rowToHz (double row) const
{
    return maxHz * std::pow (minHz / maxHz, row / overviewRows);
}

double SpectrogramView::yToHz (float y) const
{
    const auto plot = plotArea();
    const double proportion = (y - (float) plot.getY()) / (double) juce::jmax (1, plot.getHeight());
    return viewHighHz * std::pow (viewLowHz / viewHighHz, proportion);
}

float SpectrogramView::hzToY (double hz) const
{
    const auto plot = plotArea();
    const double proportion = std::log (viewHighHz / juce::jmax (hz, 1.0)) / std::log (viewHighHz / viewLowHz);
    return (float) (plot.getY() + proportion * plot.getHeight());
}

juce::Rectangle<float> SpectrogramView::boxRect() const
{
    const auto plot = plotArea().toFloat();
    const float left = sampleToX ((double) selection.startSample), right = sampleToX ((double) selection.endSample);

    if (fullBand)
        return { left, plot.getY(), right - left, plot.getHeight() };

    // A box snapped to 0 Hz is drawn down to the lowest frequency shown.
    const float top = hzToY (selection.highHz);
    const float bottom = hzToY (selection.lowHz <= 0.0 ? minHz : selection.lowHz);
    return { left, top, right - left, bottom - top };
}

bool SpectrogramView::showsHandle (Handle h) const
{
    if (! selectionActive)
        return false;

    const bool timeReach = fullBand || referenceMode == mender::Reference::time;
    switch (h)
    {
        case Handle::reachBefore:
        case Handle::reachAfter:   return timeReach;
        case Handle::reachBelow:
        case Handle::reachAbove:   return ! timeReach;
        case Handle::top:
        case Handle::bottom:
        case Handle::topLeft:
        case Handle::topRight:
        case Handle::bottomLeft:
        case Handle::bottomRight:  return ! fullBand;
        case Handle::none:
        case Handle::newBox:
        case Handle::move:
        case Handle::cursor:
        case Handle::left:
        case Handle::right:
        default:                   return true;
    }
}

juce::Point<float> SpectrogramView::getHandlePosition (Handle h) const
{
    const auto plot = plotArea().toFloat();
    const auto box = boxRect();
    const auto r = getEffectiveReach();
    const float midY = juce::jlimit (plot.getY(), plot.getBottom(), box.getCentreY());
    const float midX = box.getCentreX();
    const double low = selection.lowHz <= 0.0 ? minHz : selection.lowHz;

    switch (h)
    {
        case Handle::move:        return box.getCentre();
        case Handle::left:        return { box.getX(), midY };
        case Handle::right:       return { box.getRight(), midY };
        case Handle::top:         return { midX, box.getY() };
        case Handle::bottom:      return { midX, box.getBottom() };
        case Handle::topLeft:     return box.getTopLeft();
        case Handle::topRight:    return box.getTopRight();
        case Handle::bottomLeft:  return box.getBottomLeft();
        case Handle::bottomRight: return box.getBottomRight();
        case Handle::reachBefore: return { sampleToX ((double) selection.startSample - r.beforeSeconds * sampleRate), midY };
        case Handle::reachAfter:  return { sampleToX ((double) selection.endSample + r.afterSeconds * sampleRate), midY };
        case Handle::reachAbove:  return { midX, hzToY (selection.highHz * std::pow (2.0, r.aboveOctaves)) };
        case Handle::reachBelow:  return { midX, hzToY (low / std::pow (2.0, r.belowOctaves)) };
        case Handle::cursor:      return { sampleToX ((double) cursor), plot.getY() - 6.0f };
        case Handle::none:
        case Handle::newBox:
        default:                  return {};
    }
}

SpectrogramView::Handle SpectrogramView::hitTest (juce::Point<float> p) const
{
    if (! image.isValid())
        return Handle::none;

    if (selectionActive)
    {
        for (auto h : { Handle::reachBefore, Handle::reachAfter, Handle::reachBelow, Handle::reachAbove })
            if (showsHandle (h) && getHandlePosition (h).getDistanceFrom (p) <= capGrab)
                return h;

        const auto box = boxRect();
        const bool inX = p.x >= box.getX() - edgeGrab && p.x <= box.getRight() + edgeGrab;
        const bool inY = p.y >= box.getY() - edgeGrab && p.y <= box.getBottom() + edgeGrab;
        const float dLeft = std::abs (p.x - box.getX()), dRight = std::abs (p.x - box.getRight());
        const float dTop = std::abs (p.y - box.getY()), dBottom = std::abs (p.y - box.getBottom());

        const bool left = inY && dLeft <= edgeGrab && dLeft <= dRight;
        const bool right = inY && ! left && dRight <= edgeGrab;
        const bool top = ! fullBand && inX && dTop <= edgeGrab && dTop <= dBottom;
        const bool bottom = ! fullBand && inX && ! top && dBottom <= edgeGrab;

        if (top && left)     return Handle::topLeft;
        if (top && right)    return Handle::topRight;
        if (bottom && left)  return Handle::bottomLeft;
        if (bottom && right) return Handle::bottomRight;
        if (left)            return Handle::left;
        if (right)           return Handle::right;
        if (top)             return Handle::top;
        if (bottom)          return Handle::bottom;
        if (box.contains (p))
            return Handle::move;
    }

    const auto plot = plotArea().toFloat();
    const float dx = std::abs (p.x - sampleToX ((double) cursor));
    if ((p.y < plot.getY() && dx <= 7.0f) || dx <= 3.0f)
        return Handle::cursor;

    return Handle::newBox;
}

//==============================================================================
void SpectrogramView::resized()
{
    startTimer (detailDelayMs);
}

void SpectrogramView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15161a));
    const auto plot = plotArea();
    const auto plotF = plot.toFloat();

    if (! image.isValid())
    {
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText ("Open an audio file, or drop one here", getLocalBounds(), juce::Justification::centred, 2);
        return;
    }

    const double pixelsPerSample = plot.getWidth() / (viewEnd - viewStart);
    const double viewSpan = std::log (viewHighHz / viewLowHz);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);

        // Overview: column c is centred on sample c * hop; rows run log-spaced from maxHz down to minHz.
        const double overviewRowsToPixels = plot.getHeight() * std::log (maxHz / minHz) / (overviewRows * viewSpan);
        g.drawImageTransformed (image, juce::AffineTransform::scale ((float) (hop * pixelsPerSample), (float) overviewRowsToPixels)
                                           .translated ((float) (plot.getX() + (-0.5 * hop - viewStart) * pixelsPerSample),
                                                        (float) (plot.getY() + plot.getHeight() * std::log (viewHighHz / maxHz) / viewSpan)));

        // Detail on top, wherever it is still accurate.
        if (detail != nullptr)
        {
            if (! staleRange.isEmpty())
            {
                const float pad = (float) (detail->fftSize * pixelsPerSample);
                const float x0 = sampleToX ((double) staleRange.getStart()) - pad;
                const float x1 = sampleToX ((double) staleRange.getEnd()) + pad;
                g.excludeClipRegion (juce::Rectangle<float> (x0, plotF.getY(), x1 - x0, plotF.getHeight()).getSmallestIntegerContainer());
            }

            const auto& d = *detail;
            const double detailSpan = std::log (d.f1 / d.f0);
            g.drawImageTransformed (d.image, juce::AffineTransform::scale ((float) ((d.t1 - d.t0) / d.width * pixelsPerSample),
                                                                           (float) (detailSpan / d.height / viewSpan * plot.getHeight()))
                                                 .translated ((float) (plot.getX() + (d.t0 - viewStart) * pixelsPerSample),
                                                              (float) (plot.getY() + std::log (viewHighHz / d.f1) / viewSpan * plot.getHeight())));
        }
    }

    // Frequency axis: round values at least 22 pixels apart.
    g.setFont (juce::FontOptions (11.0f));
    float lastY = 1.0e9f;
    for (int decade = 1; decade <= 5; ++decade)
    {
        for (double m : { 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0 })
        {
            const double hz = m * std::pow (10.0, decade);
            if (hz < viewLowHz || hz > viewHighHz)
                continue;

            const float y = hzToY (hz);
            if (lastY - y < 22.0f)
                continue;
            lastY = y;

            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawHorizontalLine (juce::roundToInt (y), plotF.getX(), plotF.getRight());
            g.setColour (juce::Colours::white.withAlpha (0.65f));
            g.drawText (hzLabel (hz), 0, juce::roundToInt (y) - 7, leftGutter - 6, 14, juce::Justification::centredRight);
        }
    }

    // Time axis: pick a round step that puts ticks roughly 90 pixels apart.
    const double secondsVisible = (viewEnd - viewStart) / sampleRate;
    double step = 600.0;
    for (double candidate : { 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0 })
    {
        if (secondsVisible / candidate * 90.0 <= plot.getWidth())
        {
            step = candidate;
            break;
        }
    }

    for (double t = std::ceil (viewStart / sampleRate / step) * step; t * sampleRate < viewEnd; t += step)
    {
        const float x = sampleToX (t * sampleRate);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawVerticalLine (juce::roundToInt (x), plotF.getBottom(), plotF.getBottom() + 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.drawText (timeLabel (t, step), juce::roundToInt (x) - 40, plot.getBottom() + 4, 80, bottomGutter - 4,
                    juce::Justification::centredTop);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot);

        if (selectionActive)
        {
            const auto box = boxRect();
            const auto whisker = juce::Colours::white.withAlpha (0.85f);

            // The audio the repair learns from: a faint band, and whiskers to drag.
            if (showsHandle (Handle::reachBefore))
            {
                const auto before = getHandlePosition (Handle::reachBefore), after = getHandlePosition (Handle::reachAfter);
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRect (juce::Rectangle<float> (before.x, box.getY(), box.getX() - before.x, box.getHeight()));
                g.fillRect (juce::Rectangle<float> (box.getRight(), box.getY(), after.x - box.getRight(), box.getHeight()));
                g.setColour (whisker);
                for (auto [from, to] : { std::pair { box.getX(), before.x }, std::pair { box.getRight(), after.x } })
                {
                    g.drawLine (from, before.y, to, before.y, 1.5f);
                    g.drawLine (to, before.y - 7.0f, to, before.y + 7.0f, 2.0f);
                }
            }
            else if (showsHandle (Handle::reachAbove))
            {
                const auto above = getHandlePosition (Handle::reachAbove), below = getHandlePosition (Handle::reachBelow);
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRect (juce::Rectangle<float> (box.getX(), above.y, box.getWidth(), box.getY() - above.y));
                g.fillRect (juce::Rectangle<float> (box.getX(), box.getBottom(), box.getWidth(), below.y - box.getBottom()));
                g.setColour (whisker);
                for (auto [from, to] : { std::pair { box.getY(), above.y }, std::pair { box.getBottom(), below.y } })
                {
                    g.drawLine (above.x, from, above.x, to, 1.5f);
                    g.drawLine (above.x - 7.0f, to, above.x + 7.0f, to, 2.0f);
                }
            }

            g.setColour (juce::Colours::white.withAlpha (0.14f));
            g.fillRect (box);
            g.setColour (juce::Colours::white);
            g.drawRect (box, 1.5f);

            // Grab handles on the corners and edges that can be dragged.
            for (auto h : { Handle::topLeft, Handle::top, Handle::topRight, Handle::right,
                            Handle::bottomRight, Handle::bottom, Handle::bottomLeft, Handle::left })
            {
                if (! showsHandle (h))
                    continue;
                const auto p = getHandlePosition (h);
                g.setColour (juce::Colours::white);
                g.fillRect (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p));
                g.setColour (juce::Colour (0xff15161a));
                g.drawRect (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p), 1.0f);
            }
        }

        const float cx = sampleToX ((double) cursor);
        g.setColour (juce::Colour (0xfff2c94c));
        g.fillRect (cx - 0.75f, plotF.getY(), 1.5f, plotF.getHeight());

        if (playhead >= 0)
        {
            g.setColour (juce::Colour (0xff4fd1c5));
            g.fillRect (sampleToX ((double) playhead) - 0.75f, plotF.getY(), 1.5f, plotF.getHeight());
        }
    }

    // Cursor handle above the picture, for dragging.
    const float cx = sampleToX ((double) cursor);
    if (cx >= plotF.getX() - 1.0f && cx <= plotF.getRight() + 1.0f)
    {
        juce::Path handle;
        handle.addTriangle (cx - 7.0f, plotF.getY() - 12.0f, cx + 7.0f, plotF.getY() - 12.0f, cx, plotF.getY());
        g.setColour (juce::Colour (0xfff2c94c));
        g.fillPath (handle);
    }
}

//==============================================================================
void SpectrogramView::mouseMove (const juce::MouseEvent& e)
{
    switch (hitTest (e.position))
    {
        case Handle::left:
        case Handle::right:
        case Handle::reachBefore:
        case Handle::reachAfter:
        case Handle::cursor:      setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Handle::top:
        case Handle::bottom:
        case Handle::reachAbove:
        case Handle::reachBelow:  setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Handle::topLeft:     setMouseCursor (juce::MouseCursor::TopLeftCornerResizeCursor); break;
        case Handle::topRight:    setMouseCursor (juce::MouseCursor::TopRightCornerResizeCursor); break;
        case Handle::bottomLeft:  setMouseCursor (juce::MouseCursor::BottomLeftCornerResizeCursor); break;
        case Handle::bottomRight: setMouseCursor (juce::MouseCursor::BottomRightCornerResizeCursor); break;
        case Handle::move:        setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
        case Handle::newBox:
        case Handle::none:
        default:                  setMouseCursor (juce::MouseCursor::CrosshairCursor); break;
    }
}

void SpectrogramView::mouseDown (const juce::MouseEvent& e)
{
    dragHandle = hitTest (e.position);
    dragAnchor = e.position;
    dragStartSelection = selection;
    dragging = false;
}

void SpectrogramView::mouseDrag (const juce::MouseEvent& e)
{
    if (! image.isValid() || dragHandle == Handle::none)
        return;

    if (! dragging)
    {
        if (dragHandle != Handle::cursor && e.getDistanceFromDragStart() < 3)
            return;
        dragging = true;
    }

    const auto plot = plotArea().toFloat();
    const auto p = plot.getConstrainedPoint (e.position);
    const auto total = (double) audio->getNumSamples();

    // Dragging to the bottom of a view that shows the lowest frequencies takes the box down to 0 Hz.
    const auto edgeHz = [&] (float y)
    {
        if (y >= plot.getBottom() - 0.5f && viewLowHz <= minHz * 1.0001)
            return 0.0;
        return yToHz (y);
    };

    auto s = dragStartSelection;

    switch (dragHandle)
    {
        case Handle::cursor:
            setCursor ((juce::int64) xToSample (p.x));
            return;

        case Handle::newBox:
        {
            const auto a = plot.getConstrainedPoint (dragAnchor);
            s.startSample = (juce::int64) juce::jlimit (0.0, total, xToSample (juce::jmin (a.x, p.x)));
            s.endSample = (juce::int64) juce::jlimit (0.0, total, xToSample (juce::jmax (a.x, p.x)));
            s.lowHz = fullBand ? 0.0 : edgeHz (juce::jmax (a.y, p.y));
            s.highHz = fullBand ? maxHz : yToHz (juce::jmin (a.y, p.y));
            break;
        }

        case Handle::move:
        {
            float dx = e.position.x - dragAnchor.x, dy = e.position.y - dragAnchor.y;
            if (e.mods.isShiftDown())
                (std::abs (dx) >= std::abs (dy) ? dy : dx) = 0.0f; // keep to one axis

            const double length = (double) (s.endSample - s.startSample);
            const double start = (double) s.startSample + dx * (viewEnd - viewStart) / plot.getWidth();
            s.startSample = (juce::int64) std::llround (juce::jlimit (0.0, juce::jmax (0.0, total - length), start));
            s.endSample = s.startSample + (juce::int64) length;

            if (! fullBand && ! juce::exactlyEqual (dy, 0.0f))
            {
                const double low = juce::jmax (s.lowHz, minHz);
                const double minRatio = minHz / low, maxRatio = maxHz / s.highHz;
                const double ratio = std::exp (-dy / plot.getHeight() * std::log (viewHighHz / viewLowHz));
                const double applied = minRatio <= maxRatio ? juce::jlimit (minRatio, maxRatio, ratio) : 1.0;
                s.lowHz = low * applied;
                s.highHz *= applied;
            }
            break;
        }

        case Handle::left:
        case Handle::right:
        case Handle::top:
        case Handle::bottom:
        case Handle::topLeft:
        case Handle::topRight:
        case Handle::bottomLeft:
        case Handle::bottomRight:
        {
            const auto h = dragHandle;
            if (h == Handle::left || h == Handle::topLeft || h == Handle::bottomLeft)
                s.startSample = (juce::int64) juce::jlimit (0.0, total, xToSample (p.x));
            if (h == Handle::right || h == Handle::topRight || h == Handle::bottomRight)
                s.endSample = (juce::int64) juce::jlimit (0.0, total, xToSample (p.x));
            if (h == Handle::top || h == Handle::topLeft || h == Handle::topRight)
                s.highHz = edgeHz (p.y);
            if (h == Handle::bottom || h == Handle::bottomLeft || h == Handle::bottomRight)
                s.lowHz = edgeHz (p.y);

            // Dragging an edge past the opposite one flips the box rather than collapsing it.
            if (s.endSample < s.startSample)
                std::swap (s.startSample, s.endSample);
            if (s.highHz < s.lowHz)
                std::swap (s.lowHz, s.highHz);
            if (s.endSample == s.startSample)
                s.endSample = std::min<std::int64_t> ((std::int64_t) total, s.startSample + 1);
            if (s.highHz <= s.lowHz)
                s.highHz = juce::jmax (s.lowHz * 1.01, s.lowHz + 1.0);
            break;
        }

        case Handle::reachBefore:
            reach.beforeSeconds = juce::jlimit (minimumReach, 30.0, ((double) s.startSample - xToSample (e.position.x)) / sampleRate);
            break;
        case Handle::reachAfter:
            reach.afterSeconds = juce::jlimit (minimumReach, 30.0, (xToSample (e.position.x) - (double) s.endSample) / sampleRate);
            break;
        case Handle::reachAbove:
            reach.aboveOctaves = juce::jlimit (0.05, 4.0, std::log2 (yToHz (e.position.y) / s.highHz));
            break;
        case Handle::reachBelow:
            reach.belowOctaves = juce::jlimit (0.05, 4.0, std::log2 (juce::jmax (s.lowHz, minHz) / yToHz (e.position.y)));
            break;

        case Handle::none:
        default:
            return;
    }

    const bool reachHandle = dragHandle == Handle::reachBefore || dragHandle == Handle::reachAfter
                             || dragHandle == Handle::reachAbove || dragHandle == Handle::reachBelow;
    if (reachHandle)
    {
        repaint();
        if (onReachChanged)
            onReachChanged();
        return;
    }

    selection = s;
    selectionActive = s.endSample > s.startSample && s.highHz > s.lowHz;
    repaint();

    if (onSelectionChanged)
        onSelectionChanged();
}

void SpectrogramView::mouseUp (const juce::MouseEvent& e)
{
    const auto handle = std::exchange (dragHandle, Handle::none);
    const bool wasDragging = std::exchange (dragging, false);

    if (! image.isValid())
        return;

    if (wasDragging)
    {
        if (handle == Handle::cursor && onSeek)
            onSeek (cursor);
        return;
    }

    // A plain click (on empty space, inside the box or on the cursor) moves the play cursor.
    if (handle == Handle::newBox || handle == Handle::move || handle == Handle::cursor)
    {
        const auto plot = plotArea().toFloat();
        if (e.position.x < plot.getX() || e.position.x > plot.getRight())
            return;

        setCursor ((juce::int64) xToSample (e.position.x));
        if (onSeek)
            onSeek (cursor);
    }
}

void SpectrogramView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (selectionActive)
        zoomToSelection();
    else
        zoomToFit();
}

void SpectrogramView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! image.isValid())
        return;

    const bool frequencyAxis = e.mods.isCommandDown() || e.mods.isAltDown();
    const bool scroll = e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
    const float amount = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;

    if (frequencyAxis)
    {
        const double span = std::log (viewHighHz / viewLowHz);
        if (scroll)
        {
            const double shift = std::exp (amount * span * 0.5);
            setFrequencyView (viewLowHz * shift, viewHighHz * shift);
            return;
        }

        // Zoom around the frequency under the mouse.
        const double anchor = std::log (yToHz (e.position.y));
        const double newSpan = span * std::pow (2.0, -amount * 2.0);
        const double proportion = (std::log (viewHighHz) - anchor) / span;
        setFrequencyView (std::exp (anchor - (1.0 - proportion) * newSpan), std::exp (anchor + proportion * newSpan));
        return;
    }

    const double length = viewEnd - viewStart;
    if (scroll)
    {
        const double delta = amount * length * -0.5;
        setView (viewStart + delta, viewEnd + delta);
        return;
    }

    // Zoom around the time under the mouse.
    const double anchor = xToSample (e.position.x);
    const double newLength = length * std::pow (2.0, -amount * 2.0);
    const double proportion = (anchor - viewStart) / length;
    setView (anchor - proportion * newLength, anchor + (1.0 - proportion) * newLength);
}
