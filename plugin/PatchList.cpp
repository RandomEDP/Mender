#include "PatchList.h"

namespace
{
constexpr int formatMagic = 0x4d4e4452; // "MNDR"
constexpr int formatVersion = 1;
constexpr int maxChannels = 64;
constexpr int maxPatchSamples = 1 << 28;
} // namespace

void PatchList::copyFrom (const PatchList& other)
{
    std::vector<Patch> copy;
    {
        const juce::SpinLock::ScopedLockType guard (other.lock);
        copy = other.applied;
    }

    const juce::SpinLock::ScopedLockType guard (lock);
    applied = std::move (copy);
    undone.clear();
}

void PatchList::add (Patch patch)
{
    undone.clear();
    applied.reserve (applied.size() + 1); // allocate before taking the lock

    const juce::SpinLock::ScopedLockType guard (lock);
    applied.push_back (std::move (patch));
}

const Patch* PatchList::undo()
{
    if (applied.empty())
        return nullptr;

    undone.reserve (undone.size() + 1);
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        undone.push_back (std::move (applied.back()));
        applied.pop_back();
    }
    return &undone.back();
}

const Patch* PatchList::redo()
{
    if (undone.empty())
        return nullptr;

    applied.reserve (applied.size() + 1);
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        applied.push_back (std::move (undone.back()));
    }
    undone.pop_back();
    return &applied.back();
}

void PatchList::addPatch (const Patch& patch, juce::AudioBuffer<float>& dest, int destStart,
                          juce::int64 sourceStart, int numSamples)
{
    const auto from = juce::jmax (sourceStart, patch.start);
    const auto to = juce::jmin (sourceStart + numSamples, patch.start + patch.delta.getNumSamples());
    if (to <= from)
        return;

    const int channels = juce::jmin (dest.getNumChannels(), patch.delta.getNumChannels());
    for (int c = 0; c < channels; ++c)
        dest.addFrom (c, destStart + (int) (from - sourceStart), patch.delta, c,
                      (int) (from - patch.start), (int) (to - from));
}

bool PatchList::applyTo (juce::AudioBuffer<float>& dest, int destStart, juce::int64 sourceStart,
                         int numSamples, bool wait) const
{
    if (wait)
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        for (const auto& patch : applied)
            addPatch (patch, dest, destStart, sourceStart, numSamples);
        return true;
    }

    const juce::SpinLock::ScopedTryLockType guard (lock);
    if (! guard.isLocked())
        return false;

    for (const auto& patch : applied)
        addPatch (patch, dest, destStart, sourceStart, numSamples);
    return true;
}

void PatchList::write (juce::OutputStream& out) const
{
    // Only called on the message thread, which is the only thread that edits.
    out.writeInt (formatMagic);
    out.writeInt (formatVersion);
    out.writeInt ((int) applied.size());

    for (const auto& patch : applied)
    {
        out.writeInt64 (patch.start);
        out.writeInt (patch.delta.getNumChannels());
        out.writeInt (patch.delta.getNumSamples());
        for (int c = 0; c < patch.delta.getNumChannels(); ++c)
            out.write (patch.delta.getReadPointer (c), sizeof (float) * (size_t) patch.delta.getNumSamples());
    }
}

bool PatchList::read (juce::InputStream& in)
{
    if (in.readInt() != formatMagic || in.readInt() != formatVersion)
        return false;

    const int count = in.readInt();
    if (count < 0)
        return false;

    std::vector<Patch> loaded;
    for (int i = 0; i < count; ++i)
    {
        Patch patch;
        patch.start = in.readInt64();
        const int channels = in.readInt();
        const int samples = in.readInt();
        if (channels < 1 || channels > maxChannels || samples < 0 || samples > maxPatchSamples)
            return false;

        patch.delta.setSize (channels, samples);
        const auto bytes = (int) (sizeof (float) * (size_t) samples);
        for (int c = 0; c < channels; ++c)
            if (in.read (patch.delta.getWritePointer (c), bytes) != bytes)
                return false;

        loaded.push_back (std::move (patch));
    }

    const juce::SpinLock::ScopedLockType guard (lock);
    applied = std::move (loaded);
    undone.clear();
    return true;
}
