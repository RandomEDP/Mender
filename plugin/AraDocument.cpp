#include "AraDocument.h"

//==============================================================================
MenderModification::MenderModification (juce::ARAAudioSource* audioSource, ARA::ARAAudioModificationHostRef hostRef,
                                        const juce::ARAAudioModification* optionalModificationToClone)
    : juce::ARAAudioModification (audioSource, hostRef, optionalModificationToClone)
{
    if (auto* other = static_cast<const MenderModification*> (optionalModificationToClone))
        patches.copyFrom (other->patches);
}

void MenderModification::notifyRepairsChanged()
{
    notifyContentChanged (juce::ARAContentUpdateScopes::samplesAreAffected(), true);
    for (auto* region : getPlaybackRegions())
        region->notifyContentChanged (juce::ARAContentUpdateScopes::samplesAreAffected(), true);
}

//==============================================================================
// Plays the host's audio with the repairs added on top.
//
// The region/time arithmetic follows the PlaybackRenderer in JUCE's ARAPluginDemo
// example, which is provided under the ISC licence:
//
//   Copyright (c) Raw Material Software Limited
//   Permission to use, copy, modify, and/or distribute this software for any
//   purpose with or without fee is hereby granted provided that the above
//   copyright notice and this permission notice appear in all copies.
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
//   SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
//   OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
//   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
namespace
{
struct SampleReadingThread final : public juce::TimeSliceThread
{
    SampleReadingThread() : juce::TimeSliceThread ("Mender ARA sample reading")
    {
        startThread (Priority::high);
    }
};

class MenderPlaybackRenderer final : public juce::ARAPlaybackRenderer
{
public:
    MenderPlaybackRenderer (ARA::PlugIn::DocumentController* dc, juce::ReadWriteLock& lockIn)
        : juce::ARAPlaybackRenderer (dc), processingLock (lockIn) {}

    void prepareToPlay (double sampleRateIn, int maximumSamplesPerBlockIn, int numChannelsIn,
                        juce::AudioProcessor::ProcessingPrecision, AlwaysNonRealtime alwaysNonRealtime) override
    {
        numChannels = numChannelsIn;
        sampleRate = sampleRateIn;
        maximumSamplesPerBlock = maximumSamplesPerBlockIn;
        tempBuffer.setSize (numChannels, maximumSamplesPerBlock);
        buffered = alwaysNonRealtime == AlwaysNonRealtime::no;

        for (const auto region : getPlaybackRegions())
        {
            auto* source = region->getAudioModification()->getAudioSource();
            if (readers.find (source) != readers.end())
                continue;

            auto direct = std::make_unique<juce::ARAAudioSourceReader> (source);
            if (buffered)
            {
                // Realtime playback must not wait on the host's disk reads, so read ahead on another thread.
                const int readAhead = juce::jmax (4 * maximumSamplesPerBlock, juce::roundToInt (2.0 * sampleRate));
                auto reader = std::make_unique<juce::BufferingAudioReader> (direct.release(), *readingThread, readAhead);
                bufferedReaders[source] = reader.get();
                readers[source] = std::move (reader);
            }
            else
            {
                readers[source] = std::move (direct);
            }
        }
    }

    void releaseResources() override
    {
        readers.clear();
        bufferedReaders.clear();
        tempBuffer.setSize (0, 0);
    }

    bool processBlock (juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& position) noexcept override
    {
        const juce::ScopedTryReadLock lock (processingLock);
        if (! lock.isLocked())
            return true;

        const int numSamples = buffer.getNumSamples();
        const auto timeInSamples = position.getTimeInSamples().orFallback (0);
        const bool offline = realtime == juce::AudioProcessor::Realtime::no;
        bool success = true, renderedAny = false;

        if (position.getIsPlaying() && numSamples <= maximumSamplesPerBlock && buffer.getNumChannels() == numChannels)
        {
            const auto blockRange = juce::Range<juce::int64>::withStartAndLength (timeInSamples, numSamples);

            for (const auto& region : getPlaybackRegions())
            {
                // Where the region sits on the timeline, and which part of this block it covers.
                const auto playbackRange = region->getSampleRange (sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
                auto renderRange = blockRange.getIntersectionWith (playbackRange);
                if (renderRange.isEmpty())
                    continue;

                // The same span measured in the source audio. No time-stretching is supported.
                const juce::Range<juce::int64> sourceRange { region->getStartInAudioModificationSamples(),
                                                             region->getEndInAudioModificationSamples() };
                const auto sourceOffset = sourceRange.getStart() - playbackRange.getStart();
                renderRange = renderRange.getIntersectionWith (sourceRange.movedToStartAt (playbackRange.getStart()));
                if (renderRange.isEmpty())
                    continue;

                auto* modification = region->getAudioModification<MenderModification>();
                auto* source = modification->getAudioSource();
                const auto reader = readers.find (source);

                if (reader == readers.end() || source->getChannelCount() != numChannels
                    || ! juce::approximatelyEqual (source->getSampleRate(), sampleRate))
                {
                    success = false;
                    continue;
                }

                if (const auto b = bufferedReaders.find (source); b != bufferedReaders.end())
                    b->second->setReadTimeout (offline ? 100 : 0);

                const int count = (int) renderRange.getLength();
                const int startInBuffer = (int) (renderRange.getStart() - blockRange.getStart());
                const auto startInSource = renderRange.getStart() + sourceOffset;

                // The first region writes straight to the output; later ones are mixed in.
                auto& target = renderedAny ? tempBuffer : buffer;
                if (! reader->second->read (&target, startInBuffer, count, startInSource, true, true))
                {
                    success = false;
                    continue;
                }

                // An offline render waits for an edit in progress; live playback
                // carries on and plays that one block unrepaired.
                if (! modification->patches.isBypassed())
                    modification->patches.applyTo (target, startInBuffer, startInSource, count, offline);

                if (renderedAny)
                {
                    for (int c = 0; c < numChannels; ++c)
                        buffer.addFrom (c, startInBuffer, tempBuffer, c, startInBuffer, count);
                }
                else
                {
                    if (startInBuffer != 0)
                        buffer.clear (0, startInBuffer);
                    if (const int end = startInBuffer + count; end < numSamples)
                        buffer.clear (end, numSamples - end);
                    renderedAny = true;
                }
            }
        }

        if (! renderedAny)
            buffer.clear();

        return success;
    }

    using juce::ARAPlaybackRenderer::processBlock;

private:
    juce::ReadWriteLock& processingLock;
    juce::SharedResourcePointer<SampleReadingThread> readingThread;
    std::map<juce::ARAAudioSource*, std::unique_ptr<juce::AudioFormatReader>> readers;
    std::map<juce::ARAAudioSource*, juce::BufferingAudioReader*> bufferedReaders;
    bool buffered = true;
    int numChannels = 2, maximumSamplesPerBlock = 128;
    double sampleRate = 48000.0;
    juce::AudioBuffer<float> tempBuffer;
};
} // namespace

//==============================================================================
void MenderDocumentController::willBeginEditing (juce::ARADocument*)
{
    processingLock.enterWrite();
}

void MenderDocumentController::didEndEditing (juce::ARADocument*)
{
    processingLock.exitWrite();
}

juce::ARAAudioModification* MenderDocumentController::doCreateAudioModification (
    juce::ARAAudioSource* audioSource, ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone) noexcept
{
    return new MenderModification (audioSource, hostRef, optionalModificationToClone);
}

juce::ARAPlaybackRenderer* MenderDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new MenderPlaybackRenderer (getDocumentController(), processingLock);
}

// Saved layout: number of modifications, then for each its persistent ID and its PatchList.
bool MenderDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                       const juce::ARAStoreObjectsFilter* filter) noexcept
{
    const auto& modifications = filter->getAudioModificationsToStore<MenderModification>();

    if (! output.writeInt64 ((juce::int64) modifications.size()))
        return false;

    for (auto* modification : modifications)
    {
        // Serialise to memory first so a patch list is written in one piece.
        juce::MemoryOutputStream data;
        modification->patches.write (data);

        if (! output.writeString (modification->getPersistentID())
            || ! output.writeInt64 ((juce::int64) data.getDataSize())
            || ! output.write (data.getData(), data.getDataSize()))
            return false;
    }

    return true;
}

bool MenderDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                                           const juce::ARARestoreObjectsFilter* filter) noexcept
{
    const auto count = input.readInt64();

    for (juce::int64 i = 0; i < count; ++i)
    {
        const auto persistentID = input.readString();
        const auto size = input.readInt64();
        if (input.failed() || size < 0 || size > std::numeric_limits<int>::max())
            return false;

        juce::MemoryBlock data ((size_t) size);
        if (input.read (data.getData(), (int) size) != (int) size)
            return false;

        // The host may ask for only some objects back; the rest are skipped.
        auto* modification = filter->getAudioModificationToRestoreStateWithID<MenderModification> (persistentID.getCharPointer());
        if (modification == nullptr)
            continue;

        juce::MemoryInputStream stream (data, false);
        if (! modification->patches.read (stream))
            return false;

        modification->notifyContentChanged (juce::ARAContentUpdateScopes::samplesAreAffected(), false);
        for (auto* region : modification->getPlaybackRegions())
            region->notifyContentChanged (juce::ARAContentUpdateScopes::samplesAreAffected(), false);
    }

    return ! input.failed();
}

//==============================================================================
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<MenderDocumentController>();
}
