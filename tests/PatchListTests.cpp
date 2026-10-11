// Tests for the plug-in's repair store: applying, undo/redo and the saved format.
#include "../plugin/PatchList.h"

#include <cstdio>

namespace
{
int failures = 0;

void check (bool ok, const char* name)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (! ok)
        ++failures;
}

Patch makePatch (juce::int64 start, int length, float value)
{
    Patch p;
    p.start = start;
    p.delta.setSize (2, length);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < length; ++i)
            p.delta.setSample (c, i, value * (float) (c + 1));
    return p;
}

// Renders source samples [start, start + n) of a silent source through the list.
juce::AudioBuffer<float> render (const PatchList& list, juce::int64 start, int n, int destStart = 0)
{
    juce::AudioBuffer<float> out (2, destStart + n);
    out.clear();
    list.applyTo (out, destStart, start, n, true);
    return out;
}
} // namespace

int main()
{
    PatchList list;
    list.add (makePatch (1000, 100, 0.5f));
    list.add (makePatch (1050, 100, 0.25f));

    {
        const auto out = render (list, 900, 400);
        check (juce::exactlyEqual (out.getSample (0, 99), 0.0f) && juce::exactlyEqual (out.getSample (0, 100), 0.5f)
                   && juce::exactlyEqual (out.getSample (0, 150), 0.75f) && juce::exactlyEqual (out.getSample (0, 200), 0.25f)
                   && juce::exactlyEqual (out.getSample (0, 250), 0.0f) && juce::exactlyEqual (out.getSample (1, 150), 1.5f),
               "overlapping patches add up at the right positions");
    }
    {
        // A block that starts in the middle of a patch and is written at an offset in the output.
        const auto out = render (list, 1090, 20, 7);
        check (juce::exactlyEqual (out.getSample (0, 6), 0.0f) && juce::exactlyEqual (out.getSample (0, 7), 0.75f)
                   && juce::exactlyEqual (out.getSample (0, 16), 0.75f) && juce::exactlyEqual (out.getSample (0, 17), 0.25f),
               "partial blocks and output offsets line up");
    }
    {
        // Rendering in small blocks gives the same result as one big block.
        const auto whole = render (list, 900, 400);
        juce::AudioBuffer<float> pieces (2, 400);
        pieces.clear();
        for (int pos = 0; pos < 400; pos += 37)
            list.applyTo (pieces, pos, 900 + pos, juce::jmin (37, 400 - pos), false);
        bool same = true;
        for (int i = 0; i < 400; ++i)
            same = same && juce::exactlyEqual (whole.getSample (0, i), pieces.getSample (0, i))
                        && juce::exactlyEqual (whole.getSample (1, i), pieces.getSample (1, i));
        check (same, "block-by-block rendering matches a single pass");
    }

    const auto* undone = list.undo();
    check (undone != nullptr && undone->start == 1050 && list.size() == 1 && list.canRedo()
               && juce::exactlyEqual (render (list, 900, 400).getSample (0, 150), 0.5f),
           "undo removes the latest patch");
    check (list.redo() != nullptr && list.size() == 2 && ! list.canRedo()
               && juce::exactlyEqual (render (list, 900, 400).getSample (0, 150), 0.75f),
           "redo puts it back");
    list.undo();
    list.add (makePatch (5000, 10, 1.0f));
    check (! list.canRedo() && list.size() == 2, "a new repair clears the redo history");

    juce::MemoryOutputStream saved;
    list.write (saved);
    PatchList restored;
    juce::MemoryInputStream in (saved.getData(), saved.getDataSize(), false);
    bool same = restored.read (in) && restored.size() == list.size() && in.isExhausted();
    const auto a = render (list, 900, 4200), b = render (restored, 900, 4200);
    for (int i = 0; i < 4200 && same; ++i)
        same = juce::exactlyEqual (a.getSample (0, i), b.getSample (0, i)) && juce::exactlyEqual (a.getSample (1, i), b.getSample (1, i));
    check (same, "saving and loading reproduces the repairs exactly");

    PatchList copy;
    copy.copyFrom (list);
    check (copy.size() == 2 && juce::exactlyEqual (render (copy, 5000, 10).getSample (1, 5), 2.0f), "cloning copies the repairs");

    const char junk[] = "this is not a patch list at all";
    juce::MemoryInputStream bad (junk, sizeof (junk), false);
    PatchList untouched;
    untouched.add (makePatch (0, 4, 1.0f));
    check (! untouched.read (bad) && untouched.size() == 1, "invalid data is rejected and leaves the list alone");

    juce::MemoryInputStream cut (saved.getData(), saved.getDataSize() - 9, false);
    check (! untouched.read (cut) && untouched.size() == 1, "truncated data is rejected");

    std::printf ("\n%s\n", failures == 0 ? "All tests passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
