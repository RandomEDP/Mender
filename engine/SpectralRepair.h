// Spectral box repair.
//
// The user marks a rectangle in time and frequency. Inside it, every frequency
// bin that is louder than the surrounding audio predicts is pulled down to the
// predicted level; bins at or below that level are left alone. Phase is kept,
// and only the difference is written back, so audio outside the affected
// frames and outside the selected band is untouched.
#pragma once

#include <cstdint>

namespace mender
{
struct Selection
{
    std::int64_t startSample = 0; // inclusive
    std::int64_t endSample = 0;   // exclusive
    double lowHz = 0.0;
    double highHz = 0.0;
};

enum class Reference
{
    time,      // predict from the audio just before and just after the box
    frequency  // predict from the bands just below and just above the box
};

struct Settings
{
    int fftSize = 2048;           // power of two, 256..16384
    double strength = 1.0;        // 0 = no change, 1 = full repair
    double thresholdDb = 0.0;     // how far above the predicted level a bin may sit
    Reference reference = Reference::time;
    int referenceFrames = 8;      // frames averaged on each side (time reference)
};

struct Result
{
    bool changed = false;
    std::int64_t changedStart = 0; // inclusive
    std::int64_t changedEnd = 0;   // exclusive
};

// The range of samples a repair with these arguments may write to. Use it to
// take an undo snapshot before calling repair().
Result affectedRange (std::int64_t numSamples, const Selection&, const Settings&);

// Repairs all channels in place.
Result repair (float* const* channels, int numChannels, std::int64_t numSamples,
               double sampleRate, const Selection&, const Settings&);
} // namespace mender
