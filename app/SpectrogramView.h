#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "SpectralRepair.h"

// Zoomable spectrogram (time and frequency) with an editable selection box,
// whiskers showing how much surrounding audio the repair learns from, a
// draggable play cursor and the moving playhead.
//
// A quick overview picture is drawn at once; whenever the view stops changing,
// the visible part is redrawn at screen resolution on a background thread.
// It only reads the audio it is given; the owner does all editing, holding
// getAudioLock() for writing while it changes the audio.
class SpectrogramView : public juce::Component,
                        private juce::Timer
{
public:
    // How much audio either side of the box the repair learns from.
    struct Reach
    {
        double beforeSeconds = 0.1, afterSeconds = 0.1;
        double belowOctaves = 0.33, aboveOctaves = 0.33;
    };

    // Parts of the view the mouse can grab.
    enum class Handle
    {
        none, newBox, move, cursor,
        left, right, top, bottom, topLeft, topRight, bottomLeft, bottomRight,
        reachBefore, reachAfter, reachBelow, reachAbove
    };

    SpectrogramView();
    ~SpectrogramView() override;

    // The buffer must outlive the view (or be replaced with setAudio (nullptr, 0)).
    void setAudio (const juce::AudioBuffer<float>* buffer, double sampleRate);

    // Redraws the part of the picture covering these samples after an edit.
    void audioChanged (juce::int64 startSample, juce::int64 endSample);

    // Hold for writing while changing the audio, so the background redraw never reads it mid-change.
    juce::ReadWriteLock& getAudioLock() { return audioLock; }

    // Which channel to show: -1 shows all channels mixed together.
    void setDisplayChannel (int channel);
    int getDisplayChannel() const { return displayChannel; }

    // Levels mapped to the darkest and brightest colours. Changing them is instant.
    void setDisplayRange (double floorDb, double ceilingDb);

    bool hasSelection() const { return selectionActive; }
    mender::Selection getSelection() const { return selection; }
    void setSelection (const mender::Selection&);
    void clearSelection();

    // Full band: new boxes always cover every frequency, so only time is selected.
    void setFullBand (bool);
    bool isFullBand() const { return fullBand; }

    // Which whiskers to show: before/after for the time reference, below/above for frequency.
    void setReferenceMode (mender::Reference);

    void setReach (const Reach&);
    Reach getReach() const { return reach; }
    Reach getEffectiveReach() const;        // with the minimum below applied
    void setMinimumReach (double seconds);  // the repair always learns from at least one analysis frame
    std::function<void()> onReachChanged;

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

    // Where a handle is drawn, in this component's coordinates.
    juce::Point<float> getHandlePosition (Handle) const;
    bool isDetailCurrent() const; // the sharp redraw of the current view has arrived

    std::function<void()> onSelectionChanged;
    std::function<void (juce::int64)> onSeek; // the user moved the cursor

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Detail;
    class Renderer;

    void timerCallback() override;
    void invalidate();
    void requestDetail();
    void detailReady (std::shared_ptr<Detail>);
    int detailFftSize (int heightPixels) const;

    void rebuildImage();
    void updateMix (int firstSample, int lastSample);
    void renderColumns (int firstColumn, int lastColumn);
    void colour (juce::Image&, const std::vector<float>& levels, int rows, int firstColumn, int lastColumn) const;
    void setView (double start, double end);
    void setFrequencyView (double lowHz, double highHz);
    const float* displayData() const;

    Handle hitTest (juce::Point<float>) const;
    bool showsHandle (Handle) const;
    juce::Rectangle<float> boxRect() const;
    juce::Rectangle<int> plotArea() const;
    double xToSample (float x) const;
    float sampleToX (double sample) const;
    double yToHz (float y) const;
    float hzToY (double hz) const;
    double rowToHz (double row) const;

    const juce::AudioBuffer<float>* audio = nullptr;
    double sampleRate = 0.0;
    int displayChannel = -1;
    std::vector<float> mix; // all channels averaged, when showing all of several channels
    juce::ReadWriteLock audioLock;

    // Overview: the whole clip at modest resolution, always available.
    juce::Image image;
    std::vector<float> levels; // dB per pixel, column by column, so colours can change without recomputing
    int fftSize = 2048, hop = 512;
    double minHz = 20.0, maxHz = 20000.0;
    double floorDb = -110.0, ceilingDb = -10.0;
    std::array<juce::PixelARGB, 256> palette;

    // Detail: the visible part at screen resolution, from the background thread.
    std::unique_ptr<Renderer> renderer;
    std::shared_ptr<Detail> detail;
    std::atomic<std::uint64_t> generation { 0 }; // changes whenever pending redraws become out of date
    juce::Range<juce::int64> staleRange;         // samples edited since the detail was drawn

    double viewStart = 0.0, viewEnd = 1.0;          // visible samples
    double viewLowHz = 20.0, viewHighHz = 20000.0;  // visible frequencies

    bool selectionActive = false, fullBand = false;
    mender::Selection selection;
    mender::Reference referenceMode = mender::Reference::time;
    Reach reach;
    double minimumReach = 0.0;

    Handle dragHandle = Handle::none;
    bool dragging = false;
    juce::Point<float> dragAnchor;
    mender::Selection dragStartSelection;

    juce::int64 cursor = 0;
    juce::int64 playhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramView)
};
