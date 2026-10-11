#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "SpectralRepair.h"

// Zoomable spectrogram with a draggable time/frequency selection box.
// It only reads the audio it is given; the owner does all editing.
class SpectrogramView : public juce::Component
{
public:
    SpectrogramView();

    // The buffer must outlive the view (or be replaced with setAudio (nullptr, 0)).
    void setAudio (const juce::AudioBuffer<float>* buffer, double sampleRate);

    // Redraws the part of the picture covering these samples after an edit.
    void audioChanged (juce::int64 startSample, juce::int64 endSample);

    bool hasSelection() const { return selectionActive; }
    mender::Selection getSelection() const { return selection; }
    void setSelection (const mender::Selection&);
    void clearSelection();

    void setPlayhead (juce::int64 sample);
    void zoomToFit();
    void zoomToSelection();
    double getSampleRate() const { return sampleRate; }

    std::function<void()> onSelectionChanged;
    std::function<void (juce::int64)> onSeek; // plain click

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void rebuildImage();
    void renderColumns (int firstColumn, int lastColumn);
    void setView (double start, double end);

    juce::Rectangle<int> plotArea() const;
    double xToSample (float x) const;
    float sampleToX (double sample) const;
    double yToHz (float y) const;
    float hzToY (double hz) const;
    double rowToHz (double row) const;

    const juce::AudioBuffer<float>* audio = nullptr;
    double sampleRate = 0.0;

    juce::Image image;
    int fftSize = 2048, hop = 512;
    double minHz = 30.0, maxHz = 20000.0;
    std::array<juce::PixelARGB, 256> palette;

    double viewStart = 0.0, viewEnd = 1.0; // visible samples

    bool selectionActive = false;
    mender::Selection selection;
    juce::Point<float> dragAnchor;
    bool dragging = false;

    juce::int64 playhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramView)
};
