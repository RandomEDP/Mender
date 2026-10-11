#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "SpectrogramView.h"

// Plays the clip being edited. The audio thread never waits: if the editor is
// in the middle of writing a repair, that one block is silent.
class ClipPlayer : public juce::AudioSource
{
public:
    ClipPlayer();

    void setClip (const juce::AudioBuffer<float>* current, const juce::AudioBuffer<float>* original, double sampleRate);
    void start (juce::int64 fromSample);
    void stop() { playing = false; }
    bool isPlaying() const { return playing; }
    juce::int64 getPosition() const { return position; }
    void setPosition (juce::int64 p) { position = p; }
    void setPlayOriginal (bool shouldPlayOriginal) { playOriginal = shouldPlayOriginal; }

    juce::SpinLock& getLock() { return lock; }

    void prepareToPlay (int blockSize, double deviceRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

private:
    struct Reader : public juce::AudioSource
    {
        explicit Reader (ClipPlayer& p) : owner (p) {}
        void prepareToPlay (int, double) override {}
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
        ClipPlayer& owner;
    };

    Reader reader { *this };
    juce::ResamplingAudioSource resampler { &reader, false, 2 };

    juce::SpinLock lock;
    const juce::AudioBuffer<float>* currentClip = nullptr;
    const juce::AudioBuffer<float>* originalClip = nullptr;
    double clipRate = 0.0, outputRate = 0.0;
    std::atomic<bool> playing { false }, playOriginal { false };
    std::atomic<juce::int64> position { 0 };
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    bool loadFile (const juce::File&);
    bool saveTo (const juce::File&);
    bool repairSelection();
    void undo();
    void redo();
    void togglePlayback();

    bool hasUnsavedChanges() const { return unsaved; }
    SpectrogramView& getView() { return view; }

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    struct UndoStep
    {
        int start = 0;
        juce::AudioBuffer<float> before, after;
    };

    void timerCallback() override;
    void chooseFileToOpen();
    void chooseFileToSave();
    void applyStep (const UndoStep&, bool useAfter);
    void updateControls();
    void setStatus (const juce::String&);
    mender::Settings currentSettings() const;

    juce::AudioFormatManager formats;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    ClipPlayer player;

    juce::AudioBuffer<float> clip, original;
    double sampleRate = 0.0;
    int sourceBits = 24;
    bool sourceIsFloat = false;
    juce::File sourceFile;
    bool unsaved = false;

    std::vector<UndoStep> history;
    size_t historyPosition = 0; // steps before this index are applied

    SpectrogramView view;
    juce::TextButton openButton { "Open..." }, saveButton { "Save As..." }, playButton { "Play" },
        originalButton { "Hear original" }, undoButton { "Undo" }, redoButton { "Redo" },
        fitButton { "Zoom out" }, repairButton { "Repair" };
    juce::ComboBox referenceBox, fftBox;
    juce::Slider strengthSlider, thresholdSlider;
    juce::Label referenceLabel { {}, "Fill from" }, fftLabel { {}, "Resolution" },
        strengthLabel { {}, "Strength" }, thresholdLabel { {}, "Tolerance" }, statusLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
