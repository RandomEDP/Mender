#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "ClipPlayer.h"
#include "EditHistory.h"
#include "SpectralEdit.h"
#include "SpectrogramView.h"

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    // File
    bool loadFile (const juce::File&);
    bool saveTo (const juce::File&);
    bool hasUnsavedChanges() const { return unsaved; }

    // Editing (all act on the channel shown, or every channel when showing all)
    bool repairSelection();
    bool copySelection();
    bool pasteAtCursor();
    void undo();
    void redo();

    // Listening
    void togglePlayback();
    void playBox();
    void auditionBox();
    juce::AudioBuffer<float> makeAudition (bool bandOnly) const; // the box's audio as it would be auditioned

    // View
    void setChannel (int channel); // -1 = all
    void setFftSize (int size);
    void setDisplay (double topDb, double rangeDb);
    SpectrogramView& getView() { return view; }

    void showAudioSettings();

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    struct Clipboard
    {
        juce::AudioBuffer<float> audio; // copied region plus `context` samples each side
        juce::int64 context = 0, length = 0;
        double lowHz = 0.0, highHz = 0.0, sampleRate = 0.0;
        int fftSize = 2048;
        bool valid = false;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void chooseFileToOpen();
    void chooseFileToSave();
    bool applyEdit (const juce::String& name, juce::int64 start, juce::int64 end,
                    const std::function<bool (float* const*, int)>& edit);
    void applyStep (const EditHistory::Step&, bool useAfter);
    std::vector<int> editChannels() const;
    void rebuildChannelList();
    void updateControls();
    void setStatus (const juce::String&);
    mender::Settings currentSettings() const;
    juce::PropertiesFile& settings() { return *properties.getUserSettings(); }

    juce::ApplicationProperties properties;
    juce::AudioFormatManager formats;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    ClipPlayer player;

    juce::AudioBuffer<float> clip;
    EditHistory history;
    double sampleRate = 0.0;
    int sourceBits = 24;
    bool sourceIsFloat = false;
    juce::File sourceFile;
    juce::StringPairArray bwavMetadata; // broadcast-WAV fields to write back (timecode etc.)
    juce::MemoryBlock ixmlChunk;        // recorder metadata to write back unchanged
    juce::StringArray trackNames;
    bool unsaved = false;
    Clipboard clipboard;

    SpectrogramView view;
    juce::TextButton openButton { "Open..." }, saveButton { "Save As..." },
        playButton { "Play" }, playBoxButton { "Play box" }, auditionButton { "Audition box" },
        originalButton { "Hear original" }, undoButton { "Undo" }, redoButton { "Redo" },
        copyButton { "Copy" }, pasteButton { "Paste" }, repairButton { "Repair" },
        timeInButton { "Time +" }, timeOutButton { juce::String::fromUTF8 ("Time \xe2\x88\x92") },
        freqInButton { "Freq +" }, freqOutButton { juce::String::fromUTF8 ("Freq \xe2\x88\x92") },
        fitButton { "Fit" }, audioButton { "Audio output..." };
    juce::ComboBox channelBox, referenceBox, fftBox;
    juce::Slider strengthSlider, thresholdSlider, displayTopSlider, displayRangeSlider, levelSlider;
    juce::Label channelLabel { {}, "Channel" }, referenceLabel { {}, "Fill from" }, fftLabel { {}, "Resolution" },
        strengthLabel { {}, "Strength" }, thresholdLabel { {}, "Tolerance" }, zoomLabel { {}, "Zoom" },
        displayTopLabel { {}, "Brightness" }, displayRangeLabel { {}, "Range" }, levelLabel { {}, "Level" },
        statusLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
