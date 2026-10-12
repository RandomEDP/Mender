#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "SpectralRepair.h"

// Zoomable spectrogram (time and frequency) with a draggable selection box,
// a draggable play cursor and the moving playhead.
// It only reads the audio it is given; the owner does all editing.
class SpectrogramView : public juce::Component
{
public:
    SpectrogramView();

    // The buffer must outlive the view (or be replaced with setAudio (nullptr, 0)).
    void setAudio (const juce::AudioBuffer<float>* buffer, double sampleRate);

    // Redraws the part of the picture covering these samples after an edit.
    void audioChanged (juce::int64 startSample, juce::int64 endSample);

    // Which channel to show: -1 shows all channels mixed together.
    void setDisplayChannel (int channel);
    int getDisplayChannel() const { return displayChannel; }

    // Levels mapped to the darkest and brightest colours. Changing them is instant.
    void setDisplayRange (double floorDb, double ceilingDb);

    bool hasSelection() const { return selectionActive; }
    mender::Selection getSelection() const { return selection; }
    void setSelection (const mender::Selection&);
    void clearSelection();

    // The play cursor: where playback starts. Moved by clicking or dragging it.
    void setCursor (juce::int64 sample);
    juce::int64 getCursor() const { return cursor; }

    // The moving position during playback (-1 hides it).
    void setPlayhead (juce::int64 sample);

    void zoomToFit();
    void zoomToSelection();
    void zoomTime (double factor);      // < 1 zooms in, around the cursor if visible
    void zoomFrequency (double factor); // < 1 zooms in, around the middle of the view
    double getSampleRate() const { return sampleRate; }

    std::function<void()> onSelectionChanged;
    std::function<void (juce::int64)> onSeek; // the user moved the cursor

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void rebuildImage();
    void updateMix (int firstSample, int lastSample);
    void renderColumns (int firstColumn, int lastColumn);
    void colourColumns (int firstColumn, int lastColumn);
    void setView (double start, double end);
    void setFrequencyView (double lowHz, double highHz);
    const float* displayData() const;

    juce::Rectangle<int> plotArea() const;
    double xToSample (float x) const;
    float sampleToX (double sample) const;
    double yToHz (float y) const;
    float hzToY (double hz) const;
    double rowToHz (double row) const;
    bool isNearCursor (float x) const;

    const juce::AudioBuffer<float>* audio = nullptr;
    double sampleRate = 0.0;
    int displayChannel = -1;
    std::vector<float> mix; // all channels averaged, when showing all of several channels

    juce::Image image;
    std::vector<float> levels; // dB per pixel, so colours can change without recomputing
    int fftSize = 2048, hop = 512;
    double minHz = 20.0, maxHz = 20000.0;
    double floorDb = -110.0, ceilingDb = -10.0;
    std::array<juce::PixelARGB, 256> palette;

    double viewStart = 0.0, viewEnd = 1.0;          // visible samples
    double viewLowHz = 20.0, viewHighHz = 20000.0;  // visible frequencies

    bool selectionActive = false;
    mender::Selection selection;

    enum class Drag { none, pending, box, cursor };
    Drag drag = Drag::none;
    juce::Point<float> dragAnchor;

    juce::int64 cursor = 0;
    juce::int64 playhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramView)
};
