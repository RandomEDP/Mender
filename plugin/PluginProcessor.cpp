#include "PluginProcessor.h"

#include "PluginEditor.h"

MenderProcessor::MenderProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void MenderProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostIsPlaying = false;
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
}

void MenderProcessor::releaseResources()
{
    hostIsPlaying = false;
    releaseResourcesForARA();
}

bool MenderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void MenderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    auto* head = getPlayHead();

    if (head != nullptr)
    {
        if (const auto position = head->getPosition())
        {
            hostIsPlaying = position->getIsPlaying();
            hostTimeSeconds = position->getTimeInSeconds().orFallback (0.0);
        }
    }

    // Without ARA (loaded as an ordinary insert) the audio passes straight through.
    if (head == nullptr || ! processBlockForARA (buffer, isRealtime(), head))
        processBlockBypassed (buffer, midi);
}

double MenderProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    return getTailLengthSecondsForARA (tail) ? tail : 0.0;
}

juce::AudioProcessorEditor* MenderProcessor::createEditor()
{
    return new MenderEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MenderProcessor();
}
