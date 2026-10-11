#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

class MenderProcessor final : public juce::AudioProcessor,
                              public juce::AudioProcessorARAExtension
{
public:
    MenderProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    const juce::String getName() const override { return "Mender"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Repairs live in the ARA document, which the host saves; there is no other state.
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

    // Host transport, for drawing the play position in the editor.
    std::atomic<bool> hostIsPlaying { false };
    std::atomic<double> hostTimeSeconds { 0.0 };

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MenderProcessor)
};
