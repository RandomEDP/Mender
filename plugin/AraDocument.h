#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PatchList.h"

// The plug-in's state for one piece of audio in the host: the list of repairs.
class MenderModification final : public juce::ARAAudioModification
{
public:
    MenderModification (juce::ARAAudioSource* audioSource, ARA::ARAAudioModificationHostRef hostRef,
                        const juce::ARAAudioModification* optionalModificationToClone);

    // Tells the host the rendered audio has changed so it refreshes playback and waveforms.
    void notifyRepairsChanged();

    PatchList patches;
};

// Creates the objects above for the host and saves/restores them with its project.
class MenderDocumentController final : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    juce::ReadWriteLock& getProcessingLock() { return processingLock; }

protected:
    void willBeginEditing (juce::ARADocument*) override;
    void didEndEditing (juce::ARADocument*) override;

    juce::ARAAudioModification* doCreateAudioModification (juce::ARAAudioSource*, ARA::ARAAudioModificationHostRef,
                                                           const juce::ARAAudioModification* optionalModificationToClone) noexcept override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    bool doRestoreObjectsFromStream (juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override;
    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override;

private:
    // Held for writing while the host restructures the document, so rendering never sees it half-changed.
    juce::ReadWriteLock processingLock;
};
