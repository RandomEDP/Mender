// Band-limited copy/paste and band-limited listening.
#pragma once

#include <cstdint>

namespace mender
{
struct EditRange
{
    bool valid = false;
    std::int64_t start = 0; // inclusive
    std::int64_t end = 0;   // exclusive
};

// Extra samples to keep either side of a copied region so its edges can be
// analysed properly. Copy [start - context, end + context).
std::int64_t pasteContext (int fftSize);

// Samples that pasteBand() may write to, for taking an undo snapshot.
EditRange pasteRange (std::int64_t numSamples, std::int64_t destStart, std::int64_t length, int fftSize);

// Replaces the band [lowHz, highHz] of dest[destStart, destStart + length) with
// the same band of the copied audio. `source` holds sourceLength samples per
// channel, where source sample `context` lines up with dest sample destStart.
// Everything outside the band is kept, and the paste fades in and out over
// half an FFT frame at each end so there is no click.
// If source has fewer channels than dest, its last channel is reused.
EditRange pasteBand (float* const* dest, int numChannels, std::int64_t numSamples, double sampleRate,
                     const float* const* source, int sourceChannels, std::int64_t sourceLength, std::int64_t context,
                     std::int64_t destStart, std::int64_t length, double lowHz, double highHz, int fftSize);

// Writes only the band [lowHz, highHz] of `in` to `out` (both `length` samples),
// for listening to what is inside a selection box.
void bandPass (const float* in, float* out, std::int64_t length, double sampleRate,
               double lowHz, double highHz, int fftSize);
} // namespace mender
