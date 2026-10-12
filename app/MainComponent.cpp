#include "MainComponent.h"

#include "WavChunks.h"

#include <numeric>

namespace
{
constexpr int maxChannels = 128;
constexpr juce::int64 maxMemoryBytes = (juce::int64) 6 << 30; // clip plus display mix

juce::String formatTime (double seconds)
{
    const int minutes = (int) (seconds / 60.0);
    return juce::String (minutes) + ":" + juce::String (seconds - minutes * 60.0, 3).paddedLeft ('0', 6);
}

juce::String formatHz (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}

// Broadcast-WAV fields worth carrying through a repair, above all the timecode.
const char* const bwavKeys[] = { juce::WavAudioFormat::bwavDescription, juce::WavAudioFormat::bwavOriginator,
                                 juce::WavAudioFormat::bwavOriginatorRef, juce::WavAudioFormat::bwavOriginationDate,
                                 juce::WavAudioFormat::bwavOriginationTime, juce::WavAudioFormat::bwavTimeReference,
                                 juce::WavAudioFormat::bwavCodingHistory };

void fadeEnds (juce::AudioBuffer<float>& buffer, int fadeLength)
{
    fadeLength = juce::jmin (fadeLength, buffer.getNumSamples() / 2);
    if (fadeLength <= 0)
        return;
    buffer.applyGainRamp (0, fadeLength, 0.0f, 1.0f);
    buffer.applyGainRamp (buffer.getNumSamples() - fadeLength, fadeLength, 1.0f, 0.0f);
}
} // namespace

//==============================================================================
MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Mender";
    options.filenameSuffix = ".settings";
    options.folderName = "Mender";
    options.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters (options);

    formats.registerBasicFormats();
    const auto savedDevice = settings().getXmlValue ("audioDevice");
    deviceManager.initialise (0, 2, savedDevice.get(), true);
    deviceManager.addChangeListener (this);
    sourcePlayer.setSource (&player);
    deviceManager.addAudioCallback (&sourcePlayer);

    addAndMakeVisible (view);
    view.onSelectionChanged = [this] { updateControls(); };
    view.onSeek = [this] (juce::int64 sample)
    {
        // Moving the cursor during playback restarts playback from there.
        if (player.isPlaying() && ! player.isAuditioning())
            player.playClip (sample);
    };

    for (auto* b : { &openButton, &saveButton, &playButton, &playBoxButton, &auditionButton, &originalButton,
                     &undoButton, &redoButton, &copyButton, &pasteButton, &repairButton, &timeInButton,
                     &timeOutButton, &freqInButton, &freqOutButton, &fitButton, &audioButton })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false); // keep the keyboard shortcuts working
    }

    openButton.onClick = [this] { chooseFileToOpen(); };
    saveButton.onClick = [this] { chooseFileToSave(); };
    playButton.onClick = [this] { togglePlayback(); };
    playBoxButton.onClick = [this] { playBox(); };
    auditionButton.onClick = [this] { auditionBox(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    copyButton.onClick = [this] { copySelection(); };
    pasteButton.onClick = [this] { pasteAtCursor(); };
    repairButton.onClick = [this] { repairSelection(); };
    timeInButton.onClick = [this] { view.zoomTime (0.5); };
    timeOutButton.onClick = [this] { view.zoomTime (2.0); };
    freqInButton.onClick = [this] { view.zoomFrequency (0.5); };
    freqOutButton.onClick = [this] { view.zoomFrequency (2.0); };
    fitButton.onClick = [this] { view.zoomToFit(); };
    audioButton.onClick = [this] { showAudioSettings(); };
    repairButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f8f83));

    playButton.setTooltip ("Play from the cursor (Space). Click or drag the yellow cursor to move it.");
    playBoxButton.setTooltip ("Play the time span of the box, all frequencies (B).");
    auditionButton.setTooltip ("Play only what is inside the box: its time span and its frequencies (A).");
    copyButton.setTooltip ("Copy the frequencies inside the box (Ctrl+C).");
    pasteButton.setTooltip ("Paste the copied frequencies at the cursor, replacing that band there (Ctrl+V).");
    originalButton.setClickingTogglesState (true);
    originalButton.setTooltip ("Play the audio as it was before any repairs.");
    originalButton.onClick = [this] { player.setPlayOriginal (originalButton.getToggleState()); };

    channelBox.onChange = [this] { setChannel (channelBox.getSelectedId() - 2); };
    channelBox.setTooltip ("Which channel to show, play and edit. \"All\" edits every channel together.");

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

    displayTopSlider.setRange (0.0, 70.0, 1.0);
    displayTopSlider.setValue (settings().getDoubleValue ("displayBrightness", 20.0), juce::dontSendNotification);
    displayTopSlider.setTextValueSuffix (" dB");
    displayTopSlider.setTooltip ("Raises quiet detail towards the bright end of the colours.");
    displayRangeSlider.setRange (30.0, 160.0, 1.0);
    displayRangeSlider.setValue (settings().getDoubleValue ("displayRange", 100.0), juce::dontSendNotification);
    displayRangeSlider.setTextValueSuffix (" dB");
    displayRangeSlider.setTooltip ("How many dB the colours span. Larger shows quieter sounds; smaller shows more contrast.");
    displayTopSlider.onValueChange = displayRangeSlider.onValueChange = [this]
    {
        settings().setValue ("displayBrightness", displayTopSlider.getValue());
        settings().setValue ("displayRange", displayRangeSlider.getValue());
        view.setDisplayRange (10.0 - displayTopSlider.getValue() - displayRangeSlider.getValue(),
                              10.0 - displayTopSlider.getValue());
    };
    displayTopSlider.onValueChange();

    levelSlider.setRange (-60.0, 12.0, 0.5);
    levelSlider.setValue (settings().getDoubleValue ("playbackLevel", 0.0), juce::dontSendNotification);
    levelSlider.setTextValueSuffix (" dB");
    levelSlider.setTooltip ("Playback level. Does not change the audio that is saved.");
    levelSlider.onValueChange = [this]
    {
        settings().setValue ("playbackLevel", levelSlider.getValue());
        player.setLevel (juce::Decibels::decibelsToGain ((float) levelSlider.getValue(), -59.9f));
    };
    levelSlider.onValueChange();

    for (auto* s : { &strengthSlider, &thresholdSlider, &displayTopSlider, &displayRangeSlider, &levelSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
        s->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3a3d46));
        s->setWantsKeyboardFocus (false);
        addAndMakeVisible (s);
    }

    for (auto* c : { &channelBox, &referenceBox, &fftBox })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    for (auto* l : { &channelLabel, &referenceLabel, &fftLabel, &strengthLabel, &thresholdLabel, &zoomLabel,
                     &displayTopLabel, &displayRangeLabel, &levelLabel, &statusLabel })
    {
        l->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (l);
    }

    setWantsKeyboardFocus (true);
    setSize (1240, 760);
    rebuildChannelList();
    updateControls();
    setStatus ("Open a clip, drag a box around the noise, then press Repair.");
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&sourcePlayer);
    sourcePlayer.setSource (nullptr);
    properties.saveIfNeeded();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2026));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);
    const auto place = [] (juce::Rectangle<int>& row, juce::Component& c, int width, int gapAfter = 6)
    {
        c.setBounds (row.removeFromLeft (width));
        row.removeFromLeft (gapAfter);
    };

    auto row1 = area.removeFromTop (30);
    repairButton.setBounds (row1.removeFromRight (120));
    place (row1, openButton, 80);
    place (row1, saveButton, 92, 18);
    place (row1, playButton, 64);
    place (row1, playBoxButton, 78);
    place (row1, auditionButton, 102);
    place (row1, originalButton, 108, 18);
    place (row1, undoButton, 58);
    place (row1, redoButton, 58, 18);
    place (row1, copyButton, 58);
    place (row1, pasteButton, 58);

    area.removeFromTop (8);
    auto row2 = area.removeFromTop (26);
    place (row2, channelLabel, 56, 2);
    place (row2, channelBox, 190, 16);
    place (row2, referenceLabel, 58, 2);
    place (row2, referenceBox, 146, 16);
    place (row2, fftLabel, 70, 2);
    place (row2, fftBox, 118, 16);
    const int sliderWidth = juce::jmax (120, (row2.getWidth() - 60 - 66 - 16) / 2);
    place (row2, strengthLabel, 60, 2);
    place (row2, strengthSlider, sliderWidth, 16);
    place (row2, thresholdLabel, 66, 2);
    place (row2, thresholdSlider, juce::jmin (sliderWidth, row2.getWidth()));

    area.removeFromTop (8);
    auto row3 = area.removeFromTop (26);
    audioButton.setBounds (row3.removeFromRight (120));
    place (row3, zoomLabel, 40, 2);
    place (row3, timeInButton, 58, 4);
    place (row3, timeOutButton, 58, 10);
    place (row3, freqInButton, 58, 4);
    place (row3, freqOutButton, 58, 10);
    place (row3, fitButton, 44, 16);
    const int displayWidth = juce::jmax (110, (row3.getWidth() - 74 - 50 - 40 - 32) / 3);
    place (row3, displayTopLabel, 74, 2);
    place (row3, displayTopSlider, displayWidth, 16);
    place (row3, displayRangeLabel, 50, 2);
    place (row3, displayRangeSlider, displayWidth, 16);
    place (row3, levelLabel, 40, 2);
    place (row3, levelSlider, juce::jmin (displayWidth, row3.getWidth()));

    statusLabel.setBounds (area.removeFromBottom (24));
    area.removeFromTop (8);
    view.setBounds (area);
}

//==============================================================================
bool MainComponent::loadFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return setStatus ("Could not read " + file.getFileName() + ". Supported: WAV/BWF/polywav, AIFF, FLAC, OGG, MP3."), false;

    const int channels = (int) reader->numChannels;
    const auto length = reader->lengthInSamples;

    if (channels < 1 || channels > maxChannels)
        return setStatus (file.getFileName() + " has " + juce::String (channels) + " channels; up to "
                          + juce::String (maxChannels) + " are supported."), false;

    const auto bytes = (juce::int64) sizeof (float) * length * (channels + (channels > 1 ? 1 : 0));
    if (length <= 0 || length > ((juce::int64) 1 << 30) || bytes > maxMemoryBytes)
        return setStatus (file.getFileName() + " is too long to open in one piece at " + juce::String (channels)
                          + " channels (needs " + juce::File::descriptionOfSizeInBytes (bytes)
                          + "). Trim it to the part that needs repair first."), false;

    player.setClip (nullptr, nullptr, 0.0);
    view.setAudio (nullptr, 0.0);
    history.clear();

    try
    {
        clip.setSize (channels, (int) length);
    }
    catch (const std::bad_alloc&)
    {
        clip.setSize (0, 0);
        return setStatus ("Not enough memory to open " + file.getFileName() + "."), false;
    }

    reader->read (&clip, 0, (int) length, 0, true, true);

    sampleRate = reader->sampleRate;
    sourceBits = (int) reader->bitsPerSample;
    sourceIsFloat = reader->usesFloatingPointData;
    sourceFile = file;
    unsaved = false;

    bwavMetadata.clear();
    for (auto* key : bwavKeys)
        if (reader->metadataValues.containsKey (key))
            bwavMetadata.set (key, reader->metadataValues[key]);

    ixmlChunk = wavchunks::read (file, "iXML");
    trackNames = wavchunks::trackNames (ixmlChunk.toString(), channels);

    fftBox.setSelectedId (sampleRate > 100000.0 ? 8192 : (sampleRate > 50000.0 ? 4096 : 2048), juce::dontSendNotification);
    rebuildChannelList();
    view.setAudio (&clip, sampleRate);
    player.setClip (&clip, &history, sampleRate);
    setChannel (-1);
    updateControls();

    setStatus (file.getFileName() + "  -  " + juce::String (channels) + " ch, " + juce::String (sampleRate / 1000.0, 1)
               + " kHz, " + (sourceIsFloat ? juce::String ("32-bit float") : juce::String (sourceBits) + "-bit") + ", "
               + formatTime ((double) length / sampleRate)
               + (bwavMetadata.containsKey (juce::WavAudioFormat::bwavTimeReference) ? ", timecode kept" : "")
               + (ixmlChunk.getSize() > 0 ? ", iXML kept" : ""));
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
            std::unordered_map<juce::String, juce::String> metadata;
            for (const auto& key : bwavMetadata.getAllKeys())
                metadata[key] = bwavMetadata[key];

            // Never write fewer bits than the repaired audio needs.
            auto options = juce::AudioFormatWriterOptions()
                               .withSampleRate (sampleRate)
                               .withNumChannels (clip.getNumChannels())
                               .withBitsPerSample (sourceIsFloat ? 32 : juce::jlimit (24, 32, sourceBits))
                               .withMetadataValues (metadata);
            if (sourceIsFloat)
                options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, options))
                ok = writer->writeFromAudioSampleBuffer (clip, 0, clip.getNumSamples());
        }
    }

    ok = ok && temp.overwriteTargetFileWithTemporary();
    if (! ok)
        return setStatus ("Could not save to " + file.getFullPathName()), false;

    unsaved = false;
    juce::String note;
    if (ixmlChunk.getSize() > 0 && ! wavchunks::append (file, "iXML", ixmlChunk))
        note = " (the iXML metadata could not be kept: file is over 4 GB)";

    setStatus ("Saved " + file.getFullPathName() + note);
    return true;
}

//==============================================================================
std::vector<int> MainComponent::editChannels() const
{
    const int shown = view.getDisplayChannel();
    if (shown >= 0)
        return { shown };

    std::vector<int> all ((size_t) clip.getNumChannels());
    std::iota (all.begin(), all.end(), 0);
    return all;
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

bool MainComponent::applyEdit (const juce::String& name, juce::int64 start, juce::int64 end,
                               const std::function<bool (float* const*, int)>& edit)
{
    start = juce::jlimit<juce::int64> (0, clip.getNumSamples(), start);
    end = juce::jlimit<juce::int64> (start, clip.getNumSamples(), end);
    if (end <= start)
        return false;

    EditHistory::Step step;
    step.name = name;
    step.start = (int) start;
    step.channels = editChannels();
    const int length = (int) (end - start);
    const int count = (int) step.channels.size();

    std::vector<float*> pointers;
    step.before.setSize (count, length);
    for (int k = 0; k < count; ++k)
    {
        step.before.copyFrom (k, 0, clip, step.channels[(size_t) k], step.start, length);
        pointers.push_back (clip.getWritePointer (step.channels[(size_t) k]));
    }

    bool changed;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        changed = edit (pointers.data(), count);
    }

    if (! changed)
        return false;

    step.after.setSize (count, length);
    for (int k = 0; k < count; ++k)
        step.after.copyFrom (k, 0, clip, step.channels[(size_t) k], step.start, length);

    {
        const juce::SpinLock::ScopedLockType guard (player.getLock()); // playback reads the history
        history.push (std::move (step));
    }

    unsaved = true;
    view.audioChanged (start, end);
    updateControls();
    return true;
}

bool MainComponent::repairSelection()
{
    if (! view.hasSelection() || clip.getNumSamples() == 0)
        return false;

    const auto selection = view.getSelection();
    const auto s = currentSettings();
    const auto range = mender::affectedRange (clip.getNumSamples(), selection, s);
    const auto n = (juce::int64) clip.getNumSamples();

    const bool changed = range.changed && applyEdit ("Repair", range.changedStart, range.changedEnd,
        [&] (float* const* channels, int count)
        {
            return mender::repair (channels, count, n, sampleRate, selection, s).changed;
        });

    setStatus (changed ? "Repaired. Press Space or B to listen, or Undo to step back."
                       : "Nothing in the box is louder than its surroundings. Try a lower tolerance or the other fill direction.");
    return changed;
}

bool MainComponent::copySelection()
{
    if (! view.hasSelection() || clip.getNumSamples() == 0)
        return false;

    const auto selection = view.getSelection();
    const auto channels = editChannels();
    clipboard.fftSize = fftBox.getSelectedId();
    clipboard.context = mender::pasteContext (clipboard.fftSize);
    clipboard.length = selection.endSample - selection.startSample;
    clipboard.lowHz = selection.lowHz;
    clipboard.highHz = selection.highHz;
    clipboard.sampleRate = sampleRate;

    // The region plus some context either side, silent beyond the ends of the clip.
    const auto from = selection.startSample - clipboard.context;
    const int length = (int) (clipboard.length + 2 * clipboard.context);
    clipboard.audio.setSize ((int) channels.size(), length);
    clipboard.audio.clear();

    const auto validFrom = juce::jmax<juce::int64> (0, from);
    const auto validTo = juce::jmin<juce::int64> (clip.getNumSamples(), from + length);
    for (size_t k = 0; k < channels.size() && validTo > validFrom; ++k)
        clipboard.audio.copyFrom ((int) k, (int) (validFrom - from), clip, channels[k], (int) validFrom, (int) (validTo - validFrom));

    clipboard.valid = true;
    updateControls();
    setStatus ("Copied " + formatHz (selection.lowHz) + " to " + formatHz (selection.highHz) + " ("
               + juce::String (juce::roundToInt ((double) clipboard.length / sampleRate * 1000.0))
               + " ms). Move the cursor and Paste to put it there.");
    return true;
}

bool MainComponent::pasteAtCursor()
{
    if (! clipboard.valid || clip.getNumSamples() == 0)
        return false;

    if (! juce::approximatelyEqual (clipboard.sampleRate, sampleRate))
        return setStatus ("The copied audio is at a different sample rate from this file."), false;

    const auto destStart = view.getCursor();
    const auto n = (juce::int64) clip.getNumSamples();
    const auto range = mender::pasteRange (n, destStart, clipboard.length, clipboard.fftSize);
    if (! range.valid)
        return false;

    // Which copied channel feeds each channel being edited.
    const auto targets = editChannels();
    const int copied = clipboard.audio.getNumChannels();
    std::vector<const float*> sources;
    for (size_t k = 0; k < targets.size(); ++k)
    {
        int from = (int) k;
        if (targets.size() == 1 && copied > 1)
            from = juce::jmin (targets[0], copied - 1); // pasting into one channel: use the same channel of the copy
        sources.push_back (clipboard.audio.getReadPointer (juce::jmin (from, copied - 1)));
    }

    const bool changed = applyEdit ("Paste", range.start, range.end,
        [&] (float* const* channels, int count)
        {
            return mender::pasteBand (channels, count, n, sampleRate, sources.data(), (int) sources.size(),
                                      clipboard.audio.getNumSamples(), clipboard.context, destStart,
                                      clipboard.length, clipboard.lowHz, clipboard.highHz, clipboard.fftSize).valid;
        });

    if (changed)
    {
        view.setSelection ({ destStart, juce::jmin (n, destStart + clipboard.length), clipboard.lowHz, clipboard.highHz });
        setStatus ("Pasted. The box shows where it went.");
    }
    return changed;
}

void MainComponent::applyStep (const EditHistory::Step& step, bool useAfter)
{
    const auto& source = useAfter ? step.after : step.before;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        for (size_t k = 0; k < step.channels.size(); ++k)
            clip.copyFrom (step.channels[k], step.start, source, (int) k, 0, source.getNumSamples());
    }

    unsaved = true;
    view.audioChanged (step.start, step.start + source.getNumSamples());
    updateControls();
}

void MainComponent::undo()
{
    const EditHistory::Step* step;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        step = history.undo();
    }
    if (step != nullptr)
    {
        applyStep (*step, false);
        setStatus ("Undid " + step->name.toLowerCase() + ".");
    }
}

void MainComponent::redo()
{
    const EditHistory::Step* step;
    {
        const juce::SpinLock::ScopedLockType guard (player.getLock());
        step = history.redo();
    }
    if (step != nullptr)
    {
        applyStep (*step, true);
        setStatus ("Redid " + step->name.toLowerCase() + ".");
    }
}

//==============================================================================
void MainComponent::togglePlayback()
{
    if (clip.getNumSamples() == 0)
        return;

    if (player.isPlaying())
        player.stop();
    else
        player.playClip (view.getCursor());
}

void MainComponent::playBox()
{
    if (! view.hasSelection())
        return;
    const auto s = view.getSelection();
    player.playClip (s.startSample, s.endSample);
}

juce::AudioBuffer<float> MainComponent::makeAudition (bool bandOnly) const
{
    if (! view.hasSelection() || clip.getNumSamples() == 0)
        return {};

    const auto s = view.getSelection();
    const auto channels = editChannels();
    const int pad = bandOnly ? fftBox.getSelectedId() : 0; // context so the band filter has no edge effects
    const auto from = s.startSample - pad;
    const int length = (int) (s.endSample - s.startSample) + 2 * pad;

    // Every clip channel for the span, so the original can be rebuilt if asked for.
    juce::AudioBuffer<float> span (clip.getNumChannels(), length);
    span.clear();
    const auto validFrom = juce::jmax<juce::int64> (0, from);
    const auto validTo = juce::jmin<juce::int64> (clip.getNumSamples(), from + length);
    for (int c = 0; c < clip.getNumChannels() && validTo > validFrom; ++c)
        span.copyFrom (c, (int) (validFrom - from), clip, c, (int) validFrom, (int) (validTo - validFrom));

    if (originalButton.getToggleState())
        history.restoreOriginal (span, from, length);

    const int outLength = (int) (s.endSample - s.startSample);
    juce::AudioBuffer<float> result ((int) channels.size(), outLength);
    std::vector<float> filtered ((size_t) length);

    for (size_t k = 0; k < channels.size(); ++k)
    {
        const float* in = span.getReadPointer (channels[k]);
        if (bandOnly)
        {
            mender::bandPass (in, filtered.data(), length, sampleRate, s.lowHz, s.highHz, fftBox.getSelectedId());
            in = filtered.data();
        }
        result.copyFrom ((int) k, 0, in + pad, outLength);
    }

    fadeEnds (result, (int) (0.005 * sampleRate));
    return result;
}

void MainComponent::auditionBox()
{
    if (! view.hasSelection())
        return;

    if (player.isAuditioning())
    {
        player.stop();
        return;
    }

    player.playAudition (makeAudition (true), view.getSelection().startSample);
}

//==============================================================================
void MainComponent::setChannel (int channel)
{
    if (channel >= clip.getNumChannels())
        channel = -1;

    channelBox.setSelectedId (channel + 2, juce::dontSendNotification);
    view.setDisplayChannel (channel);
    player.setSoloChannel (channel);
}

void MainComponent::setFftSize (int size)
{
    fftBox.setSelectedId (size, juce::dontSendNotification);
}

void MainComponent::setDisplay (double topDb, double rangeDb)
{
    displayRangeSlider.setValue (rangeDb, juce::dontSendNotification);
    displayTopSlider.setValue (10.0 - topDb, juce::sendNotificationSync);
}

void MainComponent::rebuildChannelList()
{
    channelBox.clear (juce::dontSendNotification);
    const int channels = clip.getNumChannels();
    channelBox.addItem (channels > 1 ? "All channels (" + juce::String (channels) + ")" : "All channels", 1);

    for (int c = 0; c < channels; ++c)
    {
        const auto name = c < trackNames.size() ? trackNames[c] : juce::String();
        channelBox.addItem (juce::String (c + 1) + (name.isNotEmpty() ? "   " + name : juce::String()), c + 2);
    }

    channelBox.setSelectedId (1, juce::dontSendNotification);
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 0, 0, 1, 2, false, false, true, false);
    selector->setSize (520, 380);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector);
    options.dialogTitle = "Audio output";
    options.dialogBackgroundColour = juce::Colour (0xff1e2026);
    options.componentToCentreAround = this;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The output device changed: remember it for next time.
    if (const auto state = deviceManager.createStateXml())
        settings().setValue ("audioDevice", state.get());
    properties.saveIfNeeded();
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
    const auto code = key.getKeyCode();

    if (key == juce::KeyPress::spaceKey)
        togglePlayback();
    else if (mods.isCommandDown() && (code == 'Y' || (code == 'Z' && mods.isShiftDown())))
        redo();
    else if (mods.isCommandDown() && code == 'Z')
        undo();
    else if (mods.isCommandDown() && code == 'C')
        copySelection();
    else if (mods.isCommandDown() && code == 'V')
        pasteAtCursor();
    else if (mods.isCommandDown() && code == 'O')
        chooseFileToOpen();
    else if (mods.isCommandDown() && code == 'S')
        chooseFileToSave();
    else if (! mods.isCommandDown() && code == 'B')
        playBox();
    else if (! mods.isCommandDown() && code == 'A')
        auditionBox();
    else if (key == juce::KeyPress::returnKey || (! mods.isCommandDown() && code == 'R'))
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

    view.setPlayhead (player.isPlaying() ? player.getClipPosition() : -1);

    const auto text = player.isPlaying() && ! player.isAuditioning() ? "Stop" : "Play";
    if (playButton.getButtonText() != text)
        playButton.setButtonText (text);

    const auto auditionText = player.isAuditioning() ? "Stop audition" : "Audition box";
    if (auditionButton.getButtonText() != auditionText)
        auditionButton.setButtonText (auditionText);
}

void MainComponent::updateControls()
{
    const bool loaded = clip.getNumSamples() > 0;
    const bool box = loaded && view.hasSelection();

    for (auto* b : { &saveButton, &playButton, &originalButton, &timeInButton, &timeOutButton,
                     &freqInButton, &freqOutButton, &fitButton })
        b->setEnabled (loaded);
    for (auto* b : { &repairButton, &playBoxButton, &auditionButton, &copyButton })
        b->setEnabled (box);
    channelBox.setEnabled (loaded && clip.getNumChannels() > 1);
    pasteButton.setEnabled (loaded && clipboard.valid);
    undoButton.setEnabled (history.canUndo());
    redoButton.setEnabled (history.canRedo());

    if (box)
    {
        const auto s = view.getSelection();
        const double start = (double) s.startSample / sampleRate, end = (double) s.endSample / sampleRate;
        setStatus ("Box: " + formatTime (start) + " to " + formatTime (end) + " ("
                   + juce::String (juce::roundToInt ((end - start) * 1000.0)) + " ms), "
                   + formatHz (s.lowHz) + " to " + formatHz (s.highHz));
    }
}

void MainComponent::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}
