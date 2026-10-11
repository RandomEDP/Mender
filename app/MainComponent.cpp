#include "MainComponent.h"

namespace
{
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

//==============================================================================
ClipPlayer::ClipPlayer() = default;

void ClipPlayer::setClip (const juce::AudioBuffer<float>* current, const juce::AudioBuffer<float>* originalAudio, double rate)
{
    playing = false;
    position = 0;

    const juce::SpinLock::ScopedLockType guard (lock);
    currentClip = current;
    originalClip = originalAudio;
    clipRate = rate;

    if (clipRate > 0.0 && outputRate > 0.0)
        resampler.setResamplingRatio (clipRate / outputRate);
}

void ClipPlayer::start (juce::int64 fromSample)
{
    position = juce::jmax<juce::int64> (0, fromSample);
    playing = true;
}

void ClipPlayer::prepareToPlay (int blockSize, double deviceRate)
{
    {
        const juce::SpinLock::ScopedLockType guard (lock);
        outputRate = deviceRate;
        resampler.setResamplingRatio (clipRate > 0.0 ? clipRate / deviceRate : 1.0);
    }

    resampler.prepareToPlay (blockSize, deviceRate);
}

void ClipPlayer::releaseResources()
{
    resampler.releaseResources();
}

void ClipPlayer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    resampler.getNextAudioBlock (info);
}

void ClipPlayer::Reader::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    if (! owner.playing)
        return;

    const juce::SpinLock::ScopedTryLockType guard (owner.lock);
    if (! guard.isLocked())
        return;

    const auto* source = owner.playOriginal ? owner.originalClip : owner.currentClip;
    if (source == nullptr || source->getNumChannels() == 0)
        return;

    const auto pos = owner.position.load();
    const auto count = (int) juce::jmin<juce::int64> (info.numSamples, source->getNumSamples() - pos);

    if (count <= 0)
    {
        owner.playing = false;
        return;
    }

    for (int c = 0; c < info.buffer->getNumChannels(); ++c)
        info.buffer->copyFrom (c, info.startSample, *source, juce::jmin (c, source->getNumChannels() - 1), (int) pos, count);

    owner.position = pos + count;
}

//==============================================================================
MainComponent::MainComponent()
{
    formats.registerBasicFormats();
    deviceManager.initialiseWithDefaultDevices (0, 2);
    sourcePlayer.setSource (&player);
    deviceManager.addAudioCallback (&sourcePlayer);

    addAndMakeVisible (view);
    view.onSelectionChanged = [this] { updateControls(); };
    view.onSeek = [this] (juce::int64 sample)
    {
        player.setPosition (sample);
        view.setPlayhead (sample);
    };

    for (auto* b : { &openButton, &saveButton, &playButton, &originalButton, &undoButton, &redoButton, &fitButton, &repairButton })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false); // keep Space for play/stop
    }

    openButton.onClick = [this] { chooseFileToOpen(); };
    saveButton.onClick = [this] { chooseFileToSave(); };
    playButton.onClick = [this] { togglePlayback(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    fitButton.onClick = [this] { view.zoomToFit(); };
    repairButton.onClick = [this] { repairSelection(); };
    repairButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f8f83));

    originalButton.setClickingTogglesState (true);
    originalButton.onClick = [this] { player.setPlayOriginal (originalButton.getToggleState()); };

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

    setWantsKeyboardFocus (true);
    setSize (1180, 700);
    updateControls();
    setStatus ("Open a clip, drag a box around the noise, then press Repair.");
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    deviceManager.removeAudioCallback (&sourcePlayer);
    sourcePlayer.setSource (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2026));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (30);
    openButton.setBounds (top.removeFromLeft (84));
    top.removeFromLeft (6);
    saveButton.setBounds (top.removeFromLeft (96));
    top.removeFromLeft (20);
    playButton.setBounds (top.removeFromLeft (72));
    top.removeFromLeft (6);
    originalButton.setBounds (top.removeFromLeft (112));
    top.removeFromLeft (20);
    undoButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (6);
    redoButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (20);
    fitButton.setBounds (top.removeFromLeft (84));
    repairButton.setBounds (top.removeFromRight (120));

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

//==============================================================================
bool MainComponent::loadFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        setStatus ("Could not read " + file.getFileName() + ". Supported: WAV, AIFF, FLAC, OGG, MP3.");
        return false;
    }

    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) 1 << 29)
    {
        setStatus (file.getFileName() + " is empty or too long to open in one piece.");
        return false;
    }

    player.setClip (nullptr, nullptr, 0.0);
    view.setAudio (nullptr, 0.0);

    clip.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&clip, 0, (int) reader->lengthInSamples, 0, true, true);
    original.makeCopyOf (clip);

    sampleRate = reader->sampleRate;
    sourceBits = (int) reader->bitsPerSample;
    sourceIsFloat = reader->usesFloatingPointData;
    sourceFile = file;
    history.clear();
    historyPosition = 0;
    unsaved = false;

    fftBox.setSelectedId (sampleRate > 50000.0 ? 4096 : 2048, juce::dontSendNotification);
    view.setAudio (&clip, sampleRate);
    player.setClip (&clip, &original, sampleRate);
    updateControls();
    setStatus (file.getFileName() + "  -  " + juce::String (clip.getNumChannels()) + " ch, "
               + juce::String (sampleRate / 1000.0, 1) + " kHz, " + formatTime (clip.getNumSamples() / sampleRate));
    return true;
}

bool MainComponent::saveTo (const juce::File& file)
{
    if (clip.getNumSamples() == 0)
        return false;

    // Write to a temporary file first so a failed save never damages an existing file.
    juce::TemporaryFile temp (file);
    bool ok = false;

    {
        std::unique_ptr<juce::OutputStream> stream (temp.getFile().createOutputStream());
        if (stream != nullptr)
        {
            // Never write fewer bits than the repaired audio needs.
            auto options = juce::AudioFormatWriterOptions()
                               .withSampleRate (sampleRate)
                               .withNumChannels (clip.getNumChannels())
                               .withBitsPerSample (sourceIsFloat ? 32 : juce::jlimit (24, 32, sourceBits));
            if (sourceIsFloat)
                options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, options))
                ok = writer->writeFromAudioSampleBuffer (clip, 0, clip.getNumSamples());
        }
    }

    ok = ok && temp.overwriteTargetFileWithTemporary();
    if (ok)
        unsaved = false;

    setStatus (ok ? "Saved " + file.getFullPathName() : "Could not save to " + file.getFullPathName());
    return ok;
}

mender::Settings MainComponent::currentSettings() const
{
    mender::Settings s;
    s.fftSize = fftBox.getSelectedId();
    s.strength = strengthSlider.getValue() / 100.0;
    s.thresholdDb = thresholdSlider.getValue();
    s.reference = referenceBox.getSelectedId() == 2 ? mender::Reference::frequency : mender::Reference::time;
    return s;
}

bool MainComponent::repairSelection()
{
    if (! view.hasSelection() || clip.getNumSamples() == 0)
        return false;

    const auto selection = view.getSelection();
    const auto settings = currentSettings();
    const auto range = mender::affectedRange (clip.getNumSamples(), selection, settings);
    if (! range.changed)
        return false;

    UndoStep step;
    step.start = (int) range.changedStart;
    const int length = (int) (range.changedEnd - range.changedStart);
    step.before.setSize (clip.getNumChannels(), length);
    for (int c = 0; c < clip.getNumChannels(); ++c)
        step.before.copyFrom (c, 0, clip, c, step.start, length);

    mender::Result result;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        result = mender::repair (clip.getArrayOfWritePointers(), clip.getNumChannels(), clip.getNumSamples(),
                                 sampleRate, selection, settings);
    }

    if (! result.changed)
    {
        setStatus ("Nothing in the box is louder than its surroundings. Try a lower tolerance or the other fill direction.");
        return false;
    }

    step.after.setSize (clip.getNumChannels(), length);
    for (int c = 0; c < clip.getNumChannels(); ++c)
        step.after.copyFrom (c, 0, clip, c, step.start, length);

    history.resize (historyPosition);
    history.push_back (std::move (step));
    historyPosition = history.size();
    unsaved = true;

    view.audioChanged (range.changedStart, range.changedEnd);
    updateControls();
    setStatus ("Repaired. Press Space to listen, or Undo to step back.");
    return true;
}

void MainComponent::applyStep (const UndoStep& step, bool useAfter)
{
    const auto& source = useAfter ? step.after : step.before;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        for (int c = 0; c < clip.getNumChannels(); ++c)
            clip.copyFrom (c, step.start, source, c, 0, source.getNumSamples());
    }

    unsaved = true;
    view.audioChanged (step.start, step.start + source.getNumSamples());
    updateControls();
}

void MainComponent::undo()
{
    if (historyPosition > 0)
        applyStep (history[--historyPosition], false);
}

void MainComponent::redo()
{
    if (historyPosition < history.size())
        applyStep (history[historyPosition++], true);
}

void MainComponent::togglePlayback()
{
    if (clip.getNumSamples() == 0)
        return;

    if (player.isPlaying())
    {
        player.stop();
        return;
    }

    // With a box drawn, start a second before it so the repair is heard in context.
    auto from = player.getPosition();
    if (view.hasSelection())
        from = view.getSelection().startSample - (juce::int64) sampleRate;
    else if (from >= clip.getNumSamples())
        from = 0;

    player.start (from);
}

//==============================================================================
void MainComponent::chooseFileToOpen()
{
    chooser = std::make_unique<juce::FileChooser> ("Open audio clip", sourceFile.getParentDirectory(),
                                                   formats.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  loadFile (fc.getResult());
                          });
}

void MainComponent::chooseFileToSave()
{
    if (clip.getNumSamples() == 0)
        return;

    const auto suggestion = sourceFile.getSiblingFile (sourceFile.getFileNameWithoutExtension() + "_repaired.wav");
    chooser = std::make_unique<juce::FileChooser> ("Save repaired clip", suggestion, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() != juce::File())
                                  saveTo (fc.getResult().withFileExtension ("wav"));
                          });
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        loadFile (juce::File (files[0]));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (key == juce::KeyPress::spaceKey)
        togglePlayback();
    else if (mods.isCommandDown() && (key.getKeyCode() == 'Y' || (key.getKeyCode() == 'Z' && mods.isShiftDown())))
        redo();
    else if (mods.isCommandDown() && key.getKeyCode() == 'Z')
        undo();
    else if (mods.isCommandDown() && key.getKeyCode() == 'O')
        chooseFileToOpen();
    else if (mods.isCommandDown() && key.getKeyCode() == 'S')
        chooseFileToSave();
    else if (key == juce::KeyPress::returnKey || (! mods.isCommandDown() && key.getKeyCode() == 'R'))
        repairSelection();
    else if (key == juce::KeyPress::escapeKey)
        view.clearSelection();
    else
        return false;

    return true;
}

void MainComponent::timerCallback()
{
    if (clip.getNumSamples() == 0)
        return;

    view.setPlayhead (player.getPosition());
    const auto text = player.isPlaying() ? "Stop" : "Play";
    if (playButton.getButtonText() != text)
        playButton.setButtonText (text);
}

void MainComponent::updateControls()
{
    const bool loaded = clip.getNumSamples() > 0;
    saveButton.setEnabled (loaded);
    playButton.setEnabled (loaded);
    originalButton.setEnabled (loaded);
    fitButton.setEnabled (loaded);
    repairButton.setEnabled (loaded && view.hasSelection());
    undoButton.setEnabled (historyPosition > 0);
    redoButton.setEnabled (historyPosition < history.size());

    if (loaded && view.hasSelection())
    {
        const auto s = view.getSelection();
        const double start = s.startSample / sampleRate, end = s.endSample / sampleRate;
        setStatus ("Box: " + formatTime (start) + " to " + formatTime (end) + " ("
                   + juce::String (juce::roundToInt ((end - start) * 1000.0)) + " ms), "
                   + formatHz (s.lowHz) + " to " + formatHz (s.highHz));
    }
}

void MainComponent::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}
