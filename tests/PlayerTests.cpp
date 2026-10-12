// Drives the editor's playback engine the way an audio device would, without one.
#include "../app/ClipPlayer.h"

#include <cstdio>

namespace
{
int failures = 0;

void check (bool ok, const char* name, double value = 0.0)
{
    std::printf ("%s  %-60s %10.4f\n", ok ? "PASS" : "FAIL", name, value);
    if (! ok)
        ++failures;
}

// Pulls `total` samples of 2-channel output in device-sized blocks.
juce::AudioBuffer<float> pull (ClipPlayer& player, int total, int block = 480)
{
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        player.getNextAudioBlock (juce::AudioSourceChannelInfo (&out, pos, n));
    }
    return out;
}

float rms (const juce::AudioBuffer<float>& b, int ch, int from, int n)
{
    return b.getRMSLevel (ch, from, n);
}
} // namespace

int main()
{
    const double rate = 48000.0;
    const int length = 48000;

    // 8 channels, each a constant DC level 0.1 * (channel + 1), so routing is easy to read off.
    juce::AudioBuffer<float> clip (8, length);
    for (int c = 0; c < 8; ++c)
        juce::FloatVectorOperations::fill (clip.getWritePointer (c), 0.1f * (float) (c + 1), length);

    EditHistory history;
    ClipPlayer player;
    player.setClip (&clip, &history, rate);
    player.prepareToPlay (480, rate);

    // Solo channel 3 (index 2) goes to both outputs at its own level.
    player.setSoloChannel (2);
    player.playClip (0);
    auto out = pull (player, 4800);
    check (std::abs (rms (out, 0, 2400, 1000) - 0.3f) < 1e-3f && std::abs (rms (out, 1, 2400, 1000) - 0.3f) < 1e-3f,
           "soloed channel plays on both outputs", rms (out, 0, 2400, 1000));

    // All 8 channels: left gets channels 1,3,5,7 and right 2,4,6,8, each scaled by 1/sqrt(4).
    player.setSoloChannel (-1);
    player.playClip (0);
    out = pull (player, 4800);
    check (std::abs (rms (out, 0, 2400, 1000) - (0.1f + 0.3f + 0.5f + 0.7f) / 2.0f) < 1e-3f
               && std::abs (rms (out, 1, 2400, 1000) - (0.2f + 0.4f + 0.6f + 0.8f) / 2.0f) < 1e-3f,
           "eight channels are spread across left and right", rms (out, 1, 2400, 1000));

    // Level slider: -6 dB, after the smoothing has settled.
    player.setLevel (juce::Decibels::decibelsToGain (-6.0f));
    player.setSoloChannel (0);
    player.playClip (0);
    out = pull (player, 9600);
    check (std::abs (rms (out, 0, 8000, 1000) - 0.1f * juce::Decibels::decibelsToGain (-6.0f)) < 1e-3f,
           "level control scales the output", rms (out, 0, 8000, 1000));
    player.setLevel (1.0f);
    pull (player, 9600);

    // Playing a range stops at its end and reports the position.
    player.playClip (1000, 3000);
    out = pull (player, 4800);
    check (! player.isPlaying() && player.getClipPosition() == 3000 && rms (out, 0, 2500, 1000) < 1e-6f,
           "playing a box stops at its end", (double) player.getClipPosition());

    // "Hear original" rebuilds edited audio from the history.
    EditHistory::Step step;
    step.name = "Repair";
    step.start = 10000;
    step.channels = { 0 };
    step.before.setSize (1, 2000);
    step.before.copyFrom (0, 0, clip, 0, 10000, 2000);
    step.after.setSize (1, 2000);
    step.after.clear();
    history.push (std::move (step));
    clip.clear (0, 10000, 2000); // the "repaired" audio is silence

    player.playClip (10000, 12000);
    out = pull (player, 2400);
    const bool editedHeard = rms (out, 0, 200, 1500) < 1e-6f;
    player.setPlayOriginal (true);
    player.playClip (10000, 12000);
    out = pull (player, 2400);
    check (editedHeard && std::abs (rms (out, 0, 200, 1500) - 0.1f) < 1e-3f, "Hear original plays the audio before the edit",
           rms (out, 0, 200, 1500));
    player.setPlayOriginal (false);

    // Auditions play their own buffer, then stop.
    juce::AudioBuffer<float> audition (1, 1000);
    juce::FloatVectorOperations::fill (audition.getWritePointer (0), 0.25f, 1000);
    player.playAudition (std::move (audition), 5000);
    check (player.isAuditioning() && player.getClipPosition() == 5000, "audition reports its place in the clip");
    out = pull (player, 2400);
    check (std::abs (rms (out, 1, 100, 800) - 0.25f) < 1e-3f && ! player.isPlaying(), "audition plays, then stops",
           rms (out, 1, 100, 800));

    // A clip at 192 kHz played on a 48 kHz device: four clip samples per output sample.
    juce::AudioBuffer<float> hiRes (1, 192000);
    juce::FloatVectorOperations::fill (hiRes.getWritePointer (0), 0.5f, 192000);
    player.setClip (&hiRes, nullptr, 192000.0);
    player.prepareToPlay (480, 48000.0);
    player.playClip (0);
    out = pull (player, 4800);
    check (std::abs (player.getClipPosition() - 4 * 4800) < 64 && std::abs (rms (out, 0, 2400, 1000) - 0.5f) < 1e-3f,
           "192 kHz audio plays at the right speed on a 48 kHz device", (double) player.getClipPosition());

    std::printf ("\n%s\n", failures == 0 ? "All tests passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
