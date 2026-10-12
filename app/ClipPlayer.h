#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "EditHistory.h"

// Plays the clip being edited, or a separately prepared audition buffer.
// The audio thread never waits: while the editor is writing a change, that one
// block is silent.
class ClipPlayer : public juce::AudioSource
{
public:
    ClipPlayer();

    // Call with the lock free. history may be nullptr.
    void setClip (const juce::AudioBuffer<float>* clip, const EditHistory* history, double sampleRate);

    // Plays clip samples [from, to); to < 0 means to the end.
    void playClip (juce::int64 from, juce::int64 to = -1);

    // Plays a prepared buffer whose first sample is clip sample `clipPosition`
    // (used only to place the playhead).
    void playAudition (juce::AudioBuffer<float>&& buffer, juce::int64 clipPosition);

    void stop() { playing = false; }
    bool isPlaying() const { return playing; }
    bool isAuditioning() const { return playing && auditioning; }
    juce::int64 getClipPosition() const;

    void setPlayOriginal (bool b) { playOriginal = b; }
    void setSoloChannel (int channel) { soloChannel = channel; } // -1 = all channels
    void setLevel (float gain) { level = gain; }

    juce::SpinLock& getLock() { return lock; }

    void prepareToPlay (int blockSize, double deviceRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

private:
    struct Reader : public juce::AudioSource
    {
        explicit Reader (ClipPlayer& p) : owner (p) {}
        void prepareToPlay (int, double) override {}
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
        ClipPlayer& owner;
    };

    void route (const juce::AudioBuffer<float>& source, int sourceStart, int count, int solo,
                const juce::AudioSourceChannelInfo& out) const;

    static constexpr int scratchSize = 8192;

    Reader reader { *this };
    juce::ResamplingAudioSource resampler { &reader, false, 2 };

    juce::SpinLock lock;
    const juce::AudioBuffer<float>* clip = nullptr;
    const EditHistory* history = nullptr;
    juce::AudioBuffer<float> audition, scratch;
    double clipRate = 0.0, outputRate = 0.0;

    std::atomic<bool> playing { false }, auditioning { false }, playOriginal { false };
    std::atomic<juce::int64> position { 0 }, endPosition { 0 }, auditionOffset { 0 };
    std::atomic<int> soloChannel { -1 };
    std::atomic<float> level { 1.0f };
    juce::SmoothedValue<float> smoothedLevel { 1.0f };
};
