#include "SpectrogramView.h"

#include "Fft.h"

namespace
{
constexpr int imageRows = 384;
constexpr int leftGutter = 54;
constexpr int bottomGutter = 22;
constexpr double floorDb = -105.0;
constexpr double ceilingDb = -5.0;

juce::String hzLabel (double hz)
{
    const int whole = juce::roundToInt (hz);
    return whole >= 1000 ? juce::String (whole / 1000.0, whole % 1000 == 0 ? 0 : 1) + "k" : juce::String (whole);
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
        const auto c = stops[a].interpolatedWith (stops[a + 1], (float) (pos - a));
        palette[(size_t) i] = c.getPixelARGB();
    }

    setOpaque (true);
}

void SpectrogramView::setAudio (const juce::AudioBuffer<float>* buffer, double rate)
{
    audio = buffer;
    sampleRate = rate;
    selectionActive = false;
    playhead = -1;
    rebuildImage();
    zoomToFit();

    if (onSelectionChanged)
        onSelectionChanged();
}

void SpectrogramView::rebuildImage()
{
    image = {};

    if (audio == nullptr || audio->getNumSamples() == 0 || sampleRate <= 0.0)
        return;

    fftSize = sampleRate > 50000.0 ? 4096 : 2048;
    // Keep the picture to a few thousand columns however long the clip is.
    hop = juce::jmax (fftSize / 8, (int) std::ceil (audio->getNumSamples() / 6000.0));
    maxHz = sampleRate * 0.5;
    minHz = 30.0;

    const int columns = audio->getNumSamples() / hop + 1;
    image = juce::Image (juce::Image::ARGB, columns, imageRows, true, juce::SoftwareImageType());
    renderColumns (0, columns - 1);
}

void SpectrogramView::renderColumns (int firstColumn, int lastColumn)
{
    if (! image.isValid() || audio == nullptr)
        return;

    firstColumn = juce::jmax (0, firstColumn);
    lastColumn = juce::jmin (image.getWidth() - 1, lastColumn);
    if (lastColumn < firstColumn)
        return;

    const int numSamples = audio->getNumSamples();
    const int numChannels = audio->getNumChannels();
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

    juce::Image::BitmapData pixels (image, firstColumn, 0, lastColumn - firstColumn + 1, imageRows,
                                    juce::Image::BitmapData::writeOnly);

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        const int start = column * hop - half;

        for (int i = 0; i < fftSize; ++i)
        {
            const int s = start + i;
            double v = 0.0;
            if (s >= 0 && s < numSamples)
                for (int c = 0; c < numChannels; ++c)
                    v += audio->getSample (c, s);
            spec[(size_t) i] = v / numChannels * window[(size_t) i];
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

            const double db = 20.0 * std::log10 (m + 1.0e-9);
            const int index = juce::jlimit (0, 255, juce::roundToInt ((db - floorDb) / (ceilingDb - floorDb) * 255.0));
            ((juce::PixelARGB*) pixels.getPixelPointer (column - firstColumn, r))->set (palette[(size_t) index]);
        }
    }
}

void SpectrogramView::audioChanged (juce::int64 startSample, juce::int64 endSample)
{
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
    const double minLength = juce::jmin (total, (double) hop * 8.0);
    double length = juce::jlimit (minLength, juce::jmax (minLength, total), end - start);
    start = juce::jlimit (0.0, juce::jmax (0.0, total - length), start);
    viewStart = start;
    viewEnd = start + length;
    repaint();
}

void SpectrogramView::zoomToFit()
{
    setView (0.0, audio != nullptr ? (double) audio->getNumSamples() : 1.0);
}

void SpectrogramView::zoomToSelection()
{
    if (! selectionActive)
        return;

    const double length = (double) (selection.endSample - selection.startSample);
    setView ((double) selection.startSample - length * 1.5, (double) selection.endSample + length * 1.5);
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
    return rowToHz ((y - (float) plot.getY()) / (double) juce::jmax (1, plot.getHeight()) * imageRows);
}

float SpectrogramView::hzToY (double hz) const
{
    const auto plot = plotArea();
    const double row = std::log (juce::jlimit (minHz, maxHz, hz) / maxHz) / std::log (minHz / maxHz) * imageRows;
    return (float) (plot.getY() + row / imageRows * plot.getHeight());
}

//==============================================================================
void SpectrogramView::resized() {}

void SpectrogramView::paint (juce::Graphics& g)
{
    const auto background = juce::Colour (0xff15161a);
    g.fillAll (background);
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

        // Image column c is centred on sample c * hop.
        const double pixelsPerSample = plot.getWidth() / (viewEnd - viewStart);
        const auto transform = juce::AffineTransform::scale ((float) (hop * pixelsPerSample),
                                                             (float) plot.getHeight() / imageRows)
                                   .translated ((float) (plot.getX() + (-0.5 * hop - viewStart) * pixelsPerSample),
                                                (float) plot.getY());
        g.drawImageTransformed (image, transform);
    }

    // Frequency axis.
    g.setFont (juce::FontOptions (11.0f));
    for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0, 40000.0 })
    {
        if (hz <= minHz || hz >= maxHz)
            continue;

        const float y = hzToY (hz);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawHorizontalLine (juce::roundToInt (y), (float) plot.getX(), (float) plot.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.drawText (hzLabel (hz), 0, juce::roundToInt (y) - 7, leftGutter - 6, 14, juce::Justification::centredRight);
    }

    // Time axis: pick a round step that puts ticks roughly 90 pixels apart.
    const double secondsVisible = (viewEnd - viewStart) / sampleRate;
    double step = 600.0;
    for (double candidate : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0 })
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

    if (playhead >= 0)
    {
        g.setColour (juce::Colour (0xff4fd1c5));
        g.fillRect (sampleToX ((double) playhead) - 0.75f, (float) plot.getY(), 1.5f, (float) plot.getHeight());
    }
}

//==============================================================================
void SpectrogramView::mouseDown (const juce::MouseEvent& e)
{
    dragAnchor = e.position;
    dragging = false;
}

void SpectrogramView::mouseDrag (const juce::MouseEvent& e)
{
    if (! image.isValid())
        return;

    if (! dragging && e.getDistanceFromDragStart() < 4)
        return;

    dragging = true;
    const auto plot = plotArea().toFloat();
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
    if (! image.isValid() || dragging || e.mouseWasDraggedSinceMouseDown())
    {
        dragging = false;
        return;
    }

    // A plain click moves the play position.
    if (plotArea().toFloat().contains (e.position) && onSeek)
        onSeek ((juce::int64) xToSample (e.position.x));
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

    const double length = viewEnd - viewStart;

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const double delta = (e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX) * length * -0.5;
        setView (viewStart + delta, viewEnd + delta);
        return;
    }

    // Zoom around the cursor.
    const double anchor = xToSample (e.position.x);
    const double factor = std::pow (2.0, -wheel.deltaY * 2.0);
    const double newLength = length * factor;
    const double proportion = (anchor - viewStart) / length;
    setView (anchor - proportion * newLength, anchor + (1.0 - proportion) * newLength);
}
