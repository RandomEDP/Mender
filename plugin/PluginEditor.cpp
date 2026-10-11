#include "PluginEditor.h"

namespace
{
// Longest stretch of a clip shown at once. Keeps memory and loading time small.
constexpr double maxSecondsShown = 180.0;

juce::String formatTime (double seconds)
{
    const int minutes = (int) (seconds / 60.0);
    return juce::String (minutes) + ":" + juce::String (seconds - minutes * 60.0, 3).paddedLeft ('0', 6);
}

juce::String formatHz (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}
} // namespace

MenderEditor::MenderEditor (MenderProcessor& p)
    : juce::AudioProcessorEditor (&p), juce::AudioProcessorEditorARAExtension (&p), processor (p)
{
    addAndMakeVisible (view);
    view.onSelectionChanged = [this] { updateControls(); };

    for (auto* b : { &repairButton, &undoButton, &redoButton, &bypassButton, &fitButton })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false);
    }

    repairButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f8f83));
    repairButton.onClick = [this] { repairSelection(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    fitButton.onClick = [this] { view.zoomToFit(); };

    bypassButton.setClickingTogglesState (true);
    bypassButton.setTooltip ("Plays and renders the clip without the repairs. Turn it off before bouncing.");
    bypassButton.onClick = [this]
    {
        if (auto* m = modification())
        {
            m->patches.setBypassed (bypassButton.getToggleState());
            m->notifyRepairsChanged();
        }
    };

    referenceBox.addItem ("Before and after", 1);
    referenceBox.addItem ("Above and below", 2);
    referenceBox.setSelectedId (1, juce::dontSendNotification);
    referenceBox.setTooltip ("Where the replacement level comes from: the audio either side of the box in time, "
                             "or the frequencies just above and below it.");

    for (int size : { 1024, 2048, 4096, 8192, 16384 })
        fftBox.addItem (juce::String (size) + " bands", size);
    fftBox.setSelectedId (2048, juce::dontSendNotification);
    fftBox.setTooltip ("More bands separate close pitches better; fewer bands follow fast sounds better.");

    strengthSlider.setRange (0.0, 100.0, 1.0);
    strengthSlider.setValue (100.0, juce::dontSendNotification);
    strengthSlider.setTextValueSuffix (" %");
    thresholdSlider.setRange (-12.0, 12.0, 0.5);
    thresholdSlider.setValue (0.0, juce::dontSendNotification);
    thresholdSlider.setTextValueSuffix (" dB");
    thresholdSlider.setTooltip ("How far above the surrounding level a sound may sit before it is pulled down. "
                                "Lower removes more.");

    for (auto* s : { &strengthSlider, &thresholdSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
        s->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3a3d46));
        s->setWantsKeyboardFocus (false);
        addAndMakeVisible (s);
    }

    for (auto* c : { &referenceBox, &fftBox })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    for (auto* l : { &referenceLabel, &fftLabel, &strengthLabel, &thresholdLabel, &statusLabel })
    {
        l->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (l);
    }

    if (auto* editorView = getARAEditorView())
    {
        editorView->addListener (this);
        document = editorView->getDocumentController()->getDocument<juce::ARADocument>();
        if (document != nullptr)
            document->addListener (this);
        pickTarget();
    }

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (760, 360, 8000, 8000);
    setSize (1100, 600);
    updateControls();
    startTimerHz (30);
}

MenderEditor::~MenderEditor()
{
    stopTimer();
    view.setAudio (nullptr, 0.0);
    setTarget (nullptr);

    if (document != nullptr)
        document->removeListener (this);
    if (auto* editorView = getARAEditorView())
        editorView->removeListener (this);
}

MenderModification* MenderEditor::modification() const
{
    return region != nullptr ? region->getAudioModification<MenderModification>() : nullptr;
}

//==============================================================================
void MenderEditor::pickTarget()
{
    auto* editorView = getARAEditorView();
    if (editorView == nullptr)
        return;

    // 1. Whatever is selected in the host.
    const auto selected = editorView->getViewSelection().getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
    if (! selected.empty())
        return setTarget (selected.front());

    // 2. The clip this plug-in instance plays.
    if (auto* renderer = processor.getPlaybackRenderer())
        if (const auto& regions = renderer->getPlaybackRegions(); ! regions.empty())
            return setTarget (regions.front());

    // 3. Anything in the document.
    if (document != nullptr)
        for (auto* sequence : document->getRegionSequences())
            if (const auto& regions = sequence->getPlaybackRegions(); ! regions.empty())
                return setTarget (regions.front());

    setTarget (nullptr);
}

void MenderEditor::setTarget (juce::ARAPlaybackRegion* newRegion)
{
    if (newRegion == region)
        return;

    if (region != nullptr)
    {
        region->removeListener (this);
        source->removeListener (this);
    }

    region = newRegion;
    source = region != nullptr ? region->getAudioModification()->getAudioSource() : nullptr;

    if (region != nullptr)
    {
        region->addListener (this);
        source->addListener (this);
    }

    // The audio is fetched from the timer, never from inside a host callback.
    view.setAudio (nullptr, 0.0);
    working.setSize (0, 0);
    needsReload = true;
}

void MenderEditor::onNewSelection (const juce::ARAViewSelection& selection)
{
    const auto selected = selection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
    if (! selected.empty())
        setTarget (selected.front());
}

void MenderEditor::didEndEditing (juce::ARADocument*)
{
    if (region == nullptr)
        pickTarget();
    else if (region->getStartInAudioModificationSamples() != loadedStart
             || region->getEndInAudioModificationSamples() != loadedEnd)
        needsReload = true; // the clip was trimmed or slipped
}

void MenderEditor::willDestroyDocument (juce::ARADocument*)
{
    setTarget (nullptr);
    document = nullptr;
}

void MenderEditor::willDestroyPlaybackRegion (juce::ARAPlaybackRegion* destroyed)
{
    if (destroyed == region)
        setTarget (nullptr);
}

void MenderEditor::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource*, bool enabled)
{
    if (enabled && working.getNumSamples() == 0)
        needsReload = true;
}

void MenderEditor::reload()
{
    needsReload = false;
    view.setAudio (nullptr, 0.0);
    working.setSize (0, 0);
    truncated = false;

    if (! isARAEditorView())
        return setStatus ("Mender works on clips. Add it to a clip as an ARA plug-in rather than as a mixer insert.");

    if (region == nullptr)
        return setStatus ("Select a clip to repair.");

    loadedStart = region->getStartInAudioModificationSamples();
    loadedEnd = region->getEndInAudioModificationSamples();

    if (! source->isSampleAccessEnabled())
        return setStatus ("Waiting for the host to make the clip's audio available.");

    sourceRate = source->getSampleRate();
    const auto total = (juce::int64) source->getSampleCount();
    const auto start = juce::jlimit<juce::int64> (0, total, loadedStart);
    auto end = juce::jlimit<juce::int64> (start, total, loadedEnd);

    if (const auto longest = (juce::int64) (maxSecondsShown * sourceRate); end - start > longest)
    {
        end = start + longest;
        truncated = true;
    }

    const int length = (int) (end - start);
    if (length <= 0 || sourceRate <= 0.0)
        return setStatus ("This clip has no audio.");

    juce::ARAAudioSourceReader reader (source);
    working.setSize ((int) source->getChannelCount(), length);

    if (! reader.read (&working, 0, length, start, true, true))
    {
        working.setSize (0, 0);
        return setStatus ("The host could not provide this clip's audio.");
    }

    auto* m = modification();
    m->patches.applyTo (working, 0, start, length, true);
    rangeStart = start;

    fftBox.setSelectedId (sourceRate > 50000.0 ? 4096 : 2048, juce::dontSendNotification);
    bypassButton.setToggleState (m->patches.isBypassed(), juce::dontSendNotification);
    view.setAudio (&working, sourceRate);
    updateControls();
    setStatus (truncated ? "Showing the first " + formatTime (maxSecondsShown)
                               + " of this clip. Split the clip in the host to work on a later part."
                         : "Drag a box around the noise, then press Repair.");
}

//==============================================================================
void MenderEditor::repairSelection()
{
    auto* m = modification();
    if (m == nullptr || ! view.hasSelection() || working.getNumSamples() == 0)
        return;

    mender::Settings settings;
    settings.fftSize = fftBox.getSelectedId();
    settings.strength = strengthSlider.getValue() / 100.0;
    settings.thresholdDb = thresholdSlider.getValue();
    settings.reference = referenceBox.getSelectedId() == 2 ? mender::Reference::frequency : mender::Reference::time;

    const auto selection = view.getSelection();
    const auto range = mender::affectedRange (working.getNumSamples(), selection, settings);
    if (! range.changed)
        return;

    const int start = (int) range.changedStart;
    const int length = (int) (range.changedEnd - range.changedStart);
    const int channels = working.getNumChannels();

    Patch patch;
    patch.start = rangeStart + start;
    patch.delta.setSize (channels, length);
    for (int c = 0; c < channels; ++c)
        patch.delta.copyFrom (c, 0, working, c, start, length); // audio before the repair

    const auto result = mender::repair (working.getArrayOfWritePointers(), channels, working.getNumSamples(),
                                        sourceRate, selection, settings);
    if (! result.changed)
        return setStatus ("Nothing in the box is louder than its surroundings. "
                          "Try a lower tolerance or the other fill direction.");

    // delta = after - before
    patch.delta.applyGain (-1.0f);
    for (int c = 0; c < channels; ++c)
        patch.delta.addFrom (c, 0, working, c, start, length);

    m->patches.add (std::move (patch));
    m->notifyRepairsChanged();
    view.audioChanged (range.changedStart, range.changedEnd);
    updateControls();
    setStatus ("Repaired. Play the clip in the host to listen, or Undo to step back.");
}

void MenderEditor::addToWorking (const Patch& patch, float gain)
{
    const auto from = juce::jmax (rangeStart, patch.start);
    const auto to = juce::jmin (rangeStart + working.getNumSamples(), patch.start + patch.delta.getNumSamples());
    if (to <= from)
        return;

    for (int c = 0; c < juce::jmin (working.getNumChannels(), patch.delta.getNumChannels()); ++c)
        working.addFrom (c, (int) (from - rangeStart), patch.delta, c, (int) (from - patch.start), (int) (to - from), gain);

    view.audioChanged (from - rangeStart, to - rangeStart);
}

void MenderEditor::undo()
{
    if (auto* m = modification())
    {
        if (const auto* patch = m->patches.undo())
        {
            addToWorking (*patch, -1.0f);
            m->notifyRepairsChanged();
            updateControls();
        }
    }
}

void MenderEditor::redo()
{
    if (auto* m = modification())
    {
        if (const auto* patch = m->patches.redo())
        {
            addToWorking (*patch, 1.0f);
            m->notifyRepairsChanged();
            updateControls();
        }
    }
}

//==============================================================================
void MenderEditor::timerCallback()
{
    if (needsReload)
        reload();

    if (region == nullptr || working.getNumSamples() == 0)
        return;

    // Host play position, converted from timeline time to a position in the picture.
    const auto sample = (juce::int64) ((processor.hostTimeSeconds.load() - region->getStartInPlaybackTime()) * sourceRate)
                        + region->getStartInAudioModificationSamples() - rangeStart;
    view.setPlayhead (sample >= 0 && sample < working.getNumSamples() ? sample : -1);
}

void MenderEditor::updateControls()
{
    const auto* m = modification();
    const bool loaded = working.getNumSamples() > 0;
    repairButton.setEnabled (loaded && view.hasSelection());
    undoButton.setEnabled (loaded && m != nullptr && m->patches.canUndo());
    redoButton.setEnabled (loaded && m != nullptr && m->patches.canRedo());
    bypassButton.setEnabled (loaded);
    fitButton.setEnabled (loaded);

    if (loaded && view.hasSelection())
    {
        const auto s = view.getSelection();
        const double start = s.startSample / sourceRate, end = s.endSample / sourceRate;
        setStatus ("Box: " + formatTime (start) + " to " + formatTime (end) + " ("
                   + juce::String (juce::roundToInt ((end - start) * 1000.0)) + " ms), "
                   + formatHz (s.lowHz) + " to " + formatHz (s.highHz));
    }
}

void MenderEditor::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

bool MenderEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (mods.isCommandDown() && (key.getKeyCode() == 'Y' || (key.getKeyCode() == 'Z' && mods.isShiftDown())))
        redo();
    else if (mods.isCommandDown() && key.getKeyCode() == 'Z')
        undo();
    else if (key == juce::KeyPress::returnKey || (! mods.isCommandDown() && key.getKeyCode() == 'R'))
        repairSelection();
    else if (key == juce::KeyPress::escapeKey)
        view.clearSelection();
    else
        return false; // everything else, including Space, goes to the host

    return true;
}

void MenderEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2026));
}

void MenderEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (30);
    repairButton.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (20);
    undoButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (6);
    redoButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (20);
    bypassButton.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (20);
    fitButton.setBounds (top.removeFromLeft (84));

    area.removeFromTop (8);
    auto controls = area.removeFromTop (26);
    referenceLabel.setBounds (controls.removeFromLeft (58));
    referenceBox.setBounds (controls.removeFromLeft (150));
    controls.removeFromLeft (16);
    fftLabel.setBounds (controls.removeFromLeft (72));
    fftBox.setBounds (controls.removeFromLeft (120));
    controls.removeFromLeft (16);
    strengthLabel.setBounds (controls.removeFromLeft (60));
    strengthSlider.setBounds (controls.removeFromLeft (juce::jmin (220, controls.getWidth() / 2 - 70)));
    controls.removeFromLeft (16);
    thresholdLabel.setBounds (controls.removeFromLeft (66));
    thresholdSlider.setBounds (controls.removeFromLeft (juce::jmin (220, controls.getWidth())));

    statusLabel.setBounds (area.removeFromBottom (24));
    area.removeFromTop (8);
    view.setBounds (area);
}
