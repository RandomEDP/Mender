#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// One repair, stored as the difference it makes: repaired audio = source audio + delta.
struct Patch
{
    juce::int64 start = 0; // position in the audio source, in samples
    juce::AudioBuffer<float> delta;
};

// The repairs made to one clip's audio, with undo/redo. The original audio is
// never modified: the patches are added on top of it when the host plays or
// renders the clip, and they are what gets saved in the host's project.
//
// Editing happens on the message thread; applyTo() may be called from any thread.
class PatchList
{
public:
    PatchList() = default;

    void copyFrom (const PatchList& other);

    void add (Patch patch);       // also clears the redo history
    const Patch* undo();          // the patch that was removed, or nullptr
    const Patch* redo();          // the patch that was put back, or nullptr
    bool canUndo() const { return ! applied.empty(); }
    bool canRedo() const { return ! undone.empty(); }
    int size() const { return (int) applied.size(); }

    void setBypassed (bool b) { bypassed = b; }
    bool isBypassed() const { return bypassed; }

    // Adds every patch overlapping [sourceStart, sourceStart + numSamples) into
    // dest, where dest sample destStart corresponds to sourceStart.
    // With wait == false this never blocks: it returns false if an edit is in
    // progress, leaving dest untouched.
    bool applyTo (juce::AudioBuffer<float>& dest, int destStart, juce::int64 sourceStart,
                  int numSamples, bool wait) const;

    void write (juce::OutputStream&) const;
    bool read (juce::InputStream&); // replaces the contents; false if the data is not valid

private:
    static void addPatch (const Patch&, juce::AudioBuffer<float>& dest, int destStart,
                          juce::int64 sourceStart, int numSamples);

    mutable juce::SpinLock lock; // guards `applied` against the audio thread
    std::vector<Patch> applied, undone;
    std::atomic<bool> bypassed { false };

    JUCE_DECLARE_NON_COPYABLE (PatchList)
};
