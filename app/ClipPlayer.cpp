#include "ClipPlayer.h"

ClipPlayer::ClipPlayer() = default;

void ClipPlayer::setClip (const juce::AudioBuffer<float>* newClip, const EditHistory* newHistory, double rate)
{
    playing = false;
    position = 0;

    juce::AudioBuffer<float> newScratch (newClip != nullptr ? juce::jmax (1, newClip->getNumChannels()) : 1, scratchSize);
    const juce::SpinLock::ScopedLockType guard (lock);
    clip = newClip;
    history = newHistory;
    clipRate = rate;
    std::swap (scratch, newScratch);

    if (clipRate > 0.0 && outputRate > 0.0)
        resampler.setResamplingRatio (clipRate / outputRate);
}

void ClipPlayer::playClip (juce::int64 from, juce::int64 to)
{
    playing = false;
    auditioning = false;
    endPosition = to;
    position = juce::jmax<juce::int64> (0, from);
    playing = true;
}

void ClipPlayer::playAudition (juce::AudioBuffer<float>&& buffer, juce::int64 clipPosition)
{
    playing = false;
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        std::swap (audition, buffer); // the old buffer is freed when `buffer` goes out of scope, outside the lock
    }
    auditionOffset = clipPosition;
    endPosition = audition.getNumSamples();
    position = 0;
    auditioning = true;
    playing = true;
}

juce::int64 ClipPlayer::getClipPosition() const
{
    return auditioning ? auditionOffset + position : position.load();
}

void ClipPlayer::prepareToPlay (int blockSize, double deviceRate)
{
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        outputRate = deviceRate;
        resampler.setResamplingRatio (clipRate > 0.0 ? clipRate / deviceRate : 1.0);
    }

    smoothedLevel.reset (deviceRate, 0.05);
    smoothedLevel.setCurrentAndTargetValue (level);
    resampler.prepareToPlay (blockSize, deviceRate);
}

void ClipPlayer::releaseResources()
{
    resampler.releaseResources();
}

void ClipPlayer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    resampler.getNextAudioBlock (info);

    smoothedLevel.setTargetValue (level);
    if (smoothedLevel.isSmoothing())
    {
        const float start = smoothedLevel.getCurrentValue();
        smoothedLevel.skip (info.numSamples);
        info.buffer->applyGainRamp (info.startSample, info.numSamples, start, smoothedLevel.getCurrentValue());
    }
    else
    {
        info.buffer->applyGain (info.startSample, info.numSamples, smoothedLevel.getCurrentValue());
    }
}

// Sends source channels to the (usually two) outputs:
// one channel soloed -> every output; few channels -> one each;
// more channels than outputs -> spread across outputs in turn.
void ClipPlayer::route (const juce::AudioBuffer<float>& source, int sourceStart, int count, int solo,
                        const juce::AudioSourceChannelInfo& out) const
{
    const int inputs = source.getNumChannels();
    const int outputs = out.buffer->getNumChannels();
    if (inputs == 0 || outputs == 0)
        return;

    if (solo >= 0 && solo < inputs)
    {
        for (int o = 0; o < outputs; ++o)
            out.buffer->copyFrom (o, out.startSample, source, solo, sourceStart, count);
        return;
    }

    if (inputs == 1)
    {
        for (int o = 0; o < outputs; ++o)
            out.buffer->copyFrom (o, out.startSample, source, 0, sourceStart, count);
        return;
    }

    for (int o = 0; o < outputs; ++o)
    {
        const int feeding = (inputs - o + outputs - 1) / outputs; // channels o, o + outputs, ...
        if (feeding <= 0)
            continue;

        const float gain = 1.0f / std::sqrt ((float) feeding);
        for (int c = o; c < inputs; c += outputs)
            out.buffer->addFrom (o, out.startSample, source, c, sourceStart, count, gain);
    }
}

void ClipPlayer::Reader::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    auto& p = owner;
    if (! p.playing)
        return;

    const juce::SpinLock::ScopedTryLockType guard (p.lock);
    if (! guard.isLocked())
        return;

    const bool fromAudition = p.auditioning;
    const auto* source = fromAudition ? &p.audition : p.clip;
    if (source == nullptr || source->getNumChannels() == 0)
        return;

    auto pos = p.position.load();
    auto end = p.endPosition.load();
    if (end < 0 || end > source->getNumSamples())
        end = source->getNumSamples();

    int done = 0;
    while (done < info.numSamples && pos < end)
    {
        const int count = (int) std::min ({ (juce::int64) (info.numSamples - done), end - pos, (juce::int64) scratchSize });
        const juce::AudioSourceChannelInfo part (info.buffer, info.startSample + done, count);

        if (! fromAudition && p.playOriginal && p.history != nullptr && p.history->canUndo()
            && p.scratch.getNumChannels() == source->getNumChannels())
        {
            for (int c = 0; c < source->getNumChannels(); ++c)
                p.scratch.copyFrom (c, 0, *source, c, (int) pos, count);
            p.history->restoreOriginal (p.scratch, pos, count);
            p.route (p.scratch, 0, count, p.soloChannel, part);
        }
        else
        {
            p.route (*source, (int) pos, count, fromAudition ? -1 : p.soloChannel.load(), part);
        }

        pos += count;
        done += count;
    }

    p.position = pos;
    if (pos >= end)
        p.playing = false;
}
