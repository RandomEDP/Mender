#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Undo/redo for edits to a clip. Each step keeps the audio of the channels it
// touched, before and after. The applied steps also let the original audio be
// rebuilt on the fly for "Hear original", without keeping a second copy of the
// whole clip in memory.
class EditHistory
{
public:
    struct Step
    {
        juce::String name;
        int start = 0;             // first sample in the clip
        std::vector<int> channels; // clip channel for each channel of before/after
        juce::AudioBuffer<float> before, after;
    };

    void clear()
    {
        steps.clear();
        position = 0;
    }

    void push (Step step)
    {
        steps.resize (position);
        steps.push_back (std::move (step));
        position = steps.size();
    }

    bool canUndo() const { return position > 0; }
    bool canRedo() const { return position < steps.size(); }
    const Step* undo() { return canUndo() ? &steps[--position] : nullptr; }
    const Step* redo() { return canRedo() ? &steps[position++] : nullptr; }

    // Turns current audio back into the original: `block` holds every clip
    // channel for clip samples [blockStart, blockStart + numSamples).
    void restoreOriginal (juce::AudioBuffer<float>& block, juce::int64 blockStart, int numSamples) const
    {
        for (auto i = position; i-- > 0;)
        {
            const auto& step = steps[i];
            const auto from = juce::jmax<juce::int64> (blockStart, step.start);
            const auto to = juce::jmin<juce::int64> (blockStart + numSamples, step.start + step.before.getNumSamples());
            if (to <= from)
                continue;

            for (size_t k = 0; k < step.channels.size(); ++k)
                if (step.channels[k] < block.getNumChannels())
                    block.copyFrom (step.channels[k], (int) (from - blockStart), step.before, (int) k,
                                    (int) (from - step.start), (int) (to - from));
        }
    }

private:
    std::vector<Step> steps;
    size_t position = 0; // steps before this index are applied
};
