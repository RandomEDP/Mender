#include "SpectrogramView.h"

#include "Fft.h"

namespace
{
constexpr int imageRows = 768;
constexpr int leftGutter = 58;
constexpr int bottomGutter = 22;
constexpr double minFrequencyRatio = 1.25; // narrowest view: about a third of an octave

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
} // namespace

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
}

void SpectrogramView::setAudio (const juce::AudioBuffer<float>* buffer, double rate)
{
    audio = buffer;
    sampleRate = rate;
    selectionActive = false;
    cursor = 0;
    playhead = -1;
    if (audio == nullptr || displayChannel >= audio->getNumChannels())
        displayChannel = -1;

    rebuildImage();
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

    displayChannel = channel;
    rebuildImage();
    repaint();
}

void SpectrogramView::setDisplayRange (double newFloorDb, double newCeilingDb)
{
    floorDb = newFloorDb;
    ceilingDb = juce::jmax (newFloorDb + 6.0, newCeilingDb);
    if (image.isValid())
        colourColumns (0, image.getWidth() - 1);
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
    if (audio == nullptr || displayChannel >= 0 || audio->getNumChannels() < 2)
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
    // Keep the picture to a few thousand columns however long the clip is.
    hop = juce::jmax (fftSize / 8, (int) std::ceil (audio->getNumSamples() / 6000.0));
    maxHz = sampleRate * 0.5;
    minHz = 20.0;

    if (displayChannel < 0 && audio->getNumChannels() > 1)
    {
        mix.resize ((size_t) audio->getNumSamples());
        updateMix (0, audio->getNumSamples());
    }

    const int columns = audio->getNumSamples() / hop + 1;
    image = juce::Image (juce::Image::ARGB, columns, imageRows, true, juce::SoftwareImageType());
    levels.assign ((size_t) columns * imageRows, -200.0f);
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
    const int half = fftSize / 2;
    const double binHz = sampleRate / fftSize;

    std::vector<double> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / fftSize);

    // Which FFT bins feed each picture row (row 0 is the top, highest frequency).
    std::vector<double> rowLow ((size_t) imageRows), rowHigh ((size_t) imageRows);
    for (int r = 0; r < imageRows; ++r)
    {
        rowHigh[(size_t) r] = rowToHz (r) / binHz;
        rowLow[(size_t) r] = rowToHz (r + 1) / binHz;
    }

    const mender::Fft fft ((size_t) fftSize);
    std::vector<std::complex<double>> spec ((size_t) fftSize);
    std::vector<double> mags ((size_t) half + 1);
    const double scale = 4.0 / fftSize; // full-scale sine reads 0 dB

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        const int start = column * hop - half;
        for (int i = 0; i < fftSize; ++i)
        {
            const int s = start + i;
            spec[(size_t) i] = (s >= 0 && s < numSamples ? (double) data[s] : 0.0) * window[(size_t) i];
        }

        fft.forward (spec);
        for (int b = 0; b <= half; ++b)
            mags[(size_t) b] = std::abs (spec[(size_t) b]) * scale;

        for (int r = 0; r < imageRows; ++r)
        {
            const double lo = rowLow[(size_t) r], hi = rowHigh[(size_t) r];
            double m;

            if (hi - lo < 1.0)
            {
                // Low frequencies: several rows per bin, so interpolate.
                const double pos = juce::jlimit (0.0, (double) half, 0.5 * (lo + hi));
                const int b = juce::jmin ((int) pos, half - 1);
                const double frac = pos - b;
                m = mags[(size_t) b] * (1.0 - frac) + mags[(size_t) b + 1] * frac;
            }
            else
            {
                // High frequencies: several bins per row, so show the loudest.
                m = 0.0;
                const int from = juce::jlimit (0, half, (int) lo), to = juce::jlimit (0, half, (int) std::ceil (hi));
                for (int b = from; b <= to; ++b)
                    m = juce::jmax (m, mags[(size_t) b]);
            }

            levels[(size_t) column * imageRows + (size_t) r] = (float) (20.0 * std::log10 (m + 1.0e-12));
        }
    }

    colourColumns (firstColumn, lastColumn);
}

void SpectrogramView::colourColumns (int firstColumn, int lastColumn)
{
    if (! image.isValid())
        return;

    firstColumn = juce::jmax (0, firstColumn);
    lastColumn = juce::jmin (image.getWidth() - 1, lastColumn);
    if (lastColumn < firstColumn)
        return;

    const float offset = (float) floorDb;
    const float factor = (float) (255.0 / (ceilingDb - floorDb));
    juce::Image::BitmapData pixels (image, firstColumn, 0, lastColumn - firstColumn + 1, imageRows,
                                    juce::Image::BitmapData::writeOnly);

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        const float* level = levels.data() + (size_t) column * imageRows;
        for (int r = 0; r < imageRows; ++r)
        {
            const int index = juce::jlimit (0, 255, (int) ((level[r] - offset) * factor + 0.5f));
            ((juce::PixelARGB*) pixels.getPixelPointer (column - firstColumn, r))->set (palette[(size_t) index]);
        }
    }
}

void SpectrogramView::audioChanged (juce::int64 startSample, juce::int64 endSample)
{
    updateMix ((int) startSample, (int) endSample);
    renderColumns ((int) ((startSample - fftSize) / hop), (int) ((endSample + fftSize) / hop) + 1);
    repaint();
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
    const double minLength = juce::jmin (total, juce::jmax (64.0, (double) hop * 4.0));
    const double length = juce::jlimit (minLength, juce::jmax (minLength, total), end - start);
    start = juce::jlimit (0.0, juce::jmax (0.0, total - length), start);
    viewStart = start;
    viewEnd = start + length;
    repaint();
}

void SpectrogramView::setFrequencyView (double lowHz, double highHz)
{
    // Work in log frequency so zooming feels even across the range.
    const double fullSpan = std::log (maxHz / minHz);
    double span = juce::jlimit (std::log (minFrequencyRatio), fullSpan,
                                std::log (juce::jmax (highHz, 1.0) / juce::jmax (lowHz, 1.0)));
    double low = juce::jlimit (std::log (minHz), std::log (maxHz) - span, std::log (juce::jmax (lowHz, 1.0)));
    viewLowHz = std::exp (low);
    viewHighHz = std::exp (low + span);
    repaint();
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

    const double length = (double) (selection.endSample - selection.startSample);
    setView ((double) selection.startSample - length * 1.5, (double) selection.endSample + length * 1.5);
    setFrequencyView (selection.lowHz / 1.5, selection.highHz * 1.5);
}

void SpectrogramView::zoomTime (double factor)
{
    const double length = viewEnd - viewStart;
    const double anchor = (cursor >= viewStart && cursor <= viewEnd) ? (double) cursor : viewStart + 0.5 * length;
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
    return getLocalBounds().withTrimmedLeft (leftGutter).withTrimmedBottom (bottomGutter);
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
    return maxHz * std::pow (minHz / maxHz, row / imageRows);
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

bool SpectrogramView::isNearCursor (float x) const
{
    return image.isValid() && std::abs (x - sampleToX ((double) cursor)) <= 5.0f;
}

//==============================================================================
void SpectrogramView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15161a));
    const auto plot = plotArea();

    if (! image.isValid())
    {
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText ("Open an audio file, or drop one here", getLocalBounds(), juce::Justification::centred, 2);
        return;
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);

        // Image column c is centred on sample c * hop; image rows are log-spaced
        // from maxHz (row 0) to minHz, so the visible band is a linear slice of rows.
        const double pixelsPerSample = plot.getWidth() / (viewEnd - viewStart);
        const double viewSpan = std::log (viewHighHz / viewLowHz);
        const double rowsToPixels = plot.getHeight() * std::log (maxHz / minHz) / (imageRows * viewSpan);
        const auto transform = juce::AffineTransform::scale ((float) (hop * pixelsPerSample), (float) rowsToPixels)
                                   .translated ((float) (plot.getX() + (-0.5 * hop - viewStart) * pixelsPerSample),
                                                (float) (plot.getY() + plot.getHeight() * std::log (viewHighHz / maxHz) / viewSpan));
        g.drawImageTransformed (image, transform);
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
            g.drawHorizontalLine (juce::roundToInt (y), (float) plot.getX(), (float) plot.getRight());
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
        g.drawVerticalLine (juce::roundToInt (x), (float) plot.getBottom(), (float) plot.getBottom() + 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.drawText (timeLabel (t, step), juce::roundToInt (x) - 40, plot.getBottom() + 4, 80, bottomGutter - 4,
                    juce::Justification::centredTop);
    }

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (plot);

    if (selectionActive)
    {
        const juce::Rectangle<float> box (juce::Point<float> (sampleToX ((double) selection.startSample), hzToY (selection.highHz)),
                                          juce::Point<float> (sampleToX ((double) selection.endSample), hzToY (selection.lowHz)));
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.fillRect (box);
        g.setColour (juce::Colours::white);
        g.drawRect (box, 1.5f);
    }

    // Play cursor, with a handle at the top for dragging.
    const float cx = sampleToX ((double) cursor);
    g.setColour (juce::Colour (0xfff2c94c));
    g.fillRect (cx - 0.75f, (float) plot.getY(), 1.5f, (float) plot.getHeight());
    juce::Path handle;
    handle.addTriangle (cx - 6.0f, (float) plot.getY(), cx + 6.0f, (float) plot.getY(), cx, (float) plot.getY() + 9.0f);
    g.fillPath (handle);

    if (playhead >= 0)
    {
        g.setColour (juce::Colour (0xff4fd1c5));
        g.fillRect (sampleToX ((double) playhead) - 0.75f, (float) plot.getY(), 1.5f, (float) plot.getHeight());
    }
}

//==============================================================================
void SpectrogramView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isNearCursor (e.position.x) ? juce::MouseCursor::LeftRightResizeCursor
                                                : juce::MouseCursor::CrosshairCursor);
}

void SpectrogramView::mouseDown (const juce::MouseEvent& e)
{
    dragAnchor = e.position;
    drag = isNearCursor (e.position.x) ? Drag::cursor : Drag::pending;
}

void SpectrogramView::mouseDrag (const juce::MouseEvent& e)
{
    if (! image.isValid())
        return;

    const auto plot = plotArea().toFloat();

    if (drag == Drag::cursor)
    {
        setCursor ((juce::int64) xToSample (plot.getConstrainedPoint (e.position).x));
        return;
    }

    if (drag == Drag::pending && e.getDistanceFromDragStart() < 4)
        return;

    drag = Drag::box;
    const auto a = plot.getConstrainedPoint (dragAnchor);
    const auto b = plot.getConstrainedPoint (e.position);
    const auto total = (double) audio->getNumSamples();

    selection.startSample = (juce::int64) juce::jlimit (0.0, total, xToSample (juce::jmin (a.x, b.x)));
    selection.endSample = (juce::int64) juce::jlimit (0.0, total, xToSample (juce::jmax (a.x, b.x)));
    selection.lowHz = yToHz (juce::jmax (a.y, b.y));
    selection.highHz = yToHz (juce::jmin (a.y, b.y));
    selectionActive = selection.endSample > selection.startSample && selection.highHz > selection.lowHz;
    repaint();

    if (onSelectionChanged)
        onSelectionChanged();
}

void SpectrogramView::mouseUp (const juce::MouseEvent& e)
{
    const auto finished = drag;
    drag = Drag::none;

    if (! image.isValid() || finished == Drag::box)
        return;

    // A plain click, or the end of a cursor drag, moves the play cursor.
    if (finished == Drag::pending)
    {
        if (! plotArea().toFloat().contains (e.position))
            return;
        setCursor ((juce::int64) xToSample (e.position.x));
    }

    if (onSeek)
        onSeek (cursor);
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
