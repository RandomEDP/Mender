#pragma once

#include "AraDocument.h"
#include "PluginProcessor.h"
#include "SpectrogramView.h"

// The plug-in window: shows the selected clip's audio as a spectrogram and
// turns boxes drawn on it into repairs stored in that clip's ARA modification.
class MenderEditor final : public juce::AudioProcessorEditor,
                           public juce::AudioProcessorEditorARAExtension,
                           private juce::ARAEditorView::Listener,
                           private juce::ARADocument::Listener,
                           private juce::ARAPlaybackRegion::Listener,
                           private juce::ARAAudioSource::Listener,
                           private juce::Timer
{
public:
    explicit MenderEditor (MenderProcessor&);
    ~MenderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    // Host notifications.
    void onNewSelection (const juce::ARAViewSelection&) override;
    void didEndEditing (juce::ARADocument*) override;
    void willDestroyDocument (juce::ARADocument*) override;
    void willDestroyPlaybackRegion (juce::ARAPlaybackRegion*) override;
    void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource*, bool enabled) override;
    void timerCallback() override;

    void pickTarget();
    void setTarget (juce::ARAPlaybackRegion*);
    void reload();
    void repairSelection();
    void undo();
    void redo();
    void addToWorking (const Patch&, float gain);
    void updateControls();
    void setStatus (const juce::String&);
    MenderModification* modification() const;

    MenderProcessor& processor;
    juce::ARADocument* document = nullptr;
    juce::ARAPlaybackRegion* region = nullptr;
    juce::ARAAudioSource* source = nullptr;
    bool needsReload = true;

    // The part of the source audio on screen, with all repairs applied.
    juce::AudioBuffer<float> working;
    juce::int64 rangeStart = 0;                 // source sample at working sample 0
    juce::int64 loadedStart = 0, loadedEnd = 0; // region bounds when loaded
    double sourceRate = 0.0;
    bool truncated = false;

    SpectrogramView view;
    juce::TextButton repairButton { "Repair" }, undoButton { "Undo" }, redoButton { "Redo" },
        bypassButton { "Bypass repairs" }, fitButton { "Zoom out" };
    juce::ComboBox referenceBox, fftBox;
    juce::Slider strengthSlider, thresholdSlider;
    juce::Label referenceLabel { {}, "Fill from" }, fftLabel { {}, "Resolution" },
        strengthLabel { {}, "Strength" }, thresholdLabel { {}, "Tolerance" }, statusLabel;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MenderEditor)
};
