# Mender (working title)

A spectral repair editor: open a clip, drag a box around an unwanted sound on
the spectrogram, press Repair, save the result as a new file.

Written from scratch from published signal-processing techniques. It contains
no code from any other product.

## What is here

| Folder | Contents |
|---|---|
| `engine/` | The repair algorithm in plain C++17 with no dependencies |
| `plugin/` | VST3 + ARA2 plug-in: repairs a clip in place inside the host |
| `app/` | Standalone editor, and the spectrogram view both programs share |
| `tests/` | Engine and repair-store tests (`ctest`) |
| `.github/workflows/` | Builds Windows and macOS downloads on every push |

## The plug-in

Mender is an ARA plug-in: it works on a clip, not on a mixer strip. The host
hands it the clip's audio, you draw boxes and repair, and the host plays and
renders the repaired audio. The original file is never changed; the repairs
are saved inside the host's project.

Install on Windows: copy the whole `Mender.vst3` folder into
`C:\Program Files\Common Files\VST3\` and rescan plug-ins in the host.

- The window shows the selected clip, up to the first three minutes of it.
- **Repair**, **Undo**, **Redo** act on that clip. Play it from the host to listen.
- **Bypass repairs** plays the clip untouched for comparison. Turn it off
  before bouncing.
- Bounce or render the clip in the host to get the repaired audio as a new clip.
- Clips must be at the project sample rate; time-stretched clips are not supported.

## The standalone editor

Opens WAV/BWF (including multichannel polywavs from field recorders), AIFF and
FLAC, up to 128 channels and 192 kHz.

**Looking**
- Mouse wheel zooms time; Ctrl/Cmd + wheel zooms frequency. Shift scrolls.
  The Zoom buttons do the same; Fit shows everything; double-click zooms to the box.
- **Brightness** and **Range** change how loud a sound must be to show, instantly.
- **Channel** shows one channel (with its recorder track name) or all of them.
  Editing and playback follow it: "All" edits every channel together.

**Listening**
- The yellow **cursor** marks where playback starts: click to place it or drag
  its handle. Moving it during playback restarts from there.
- **Play** (Space) from the cursor, **Play box** (B) the box's time span,
  **Audition box** (A) only what is inside the box, its frequencies included.
- **Hear original** plays the audio as it was before any edits.
- **Level** sets playback volume; **Audio output...** picks the device and outputs.
  Both are remembered.

**Editing**
- Drag a box around the noise and press **Repair** (Enter or R).
- **Copy** (Ctrl+C) takes the frequencies inside the box; **Paste** (Ctrl+V)
  replaces that band at the cursor with them, fading in and out. Use it to lay
  clean room tone over a cough.
- **Undo/Redo** with Ctrl+Z / Ctrl+Y.
- **Save As** writes a new WAV at the source sample rate, 24-bit or better,
  keeping the broadcast-WAV timecode and the recorder's iXML metadata.

Repair controls: **Fill from** *Before and after* suits short noises over steady
sound, *Above and below* longer noises in a narrow band. **Resolution**: more
bands separate close pitches, fewer follow fast sounds. **Tolerance**: lower
removes more.

Files are opened whole, so very long multichannel files may need trimming
first (the limit is about 6 GB of audio in memory, e.g. 8 channels at 192 kHz
for about 15 minutes).

## Building

Needs CMake 3.22+ and a C++17 compiler (Visual Studio 2022 on Windows, Xcode on
macOS). JUCE and the ARA SDK are downloaded automatically.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
    ctest --test-dir build -C Release

The editor is written to `build/Mender_artefacts/Release/` and the plug-in to
`build/MenderPlugin_artefacts/Release/VST3/`.

## Roadmap

1. Standalone editor (done).
2. VST3 + ARA2 plug-in (first version here; needs testing in Pyramix).
3. Better repairs where the noise overlaps wanted partials, harmonic selection,
   click detection.

## Licensing notes

JUCE is dual-licensed: AGPLv3 or a commercial JUCE licence. Before selling or
sharing builds, check the current JUCE terms at https://juce.com and choose one.
The ARA SDK is Apache 2.0. The plug-in's playback code follows JUCE's ISC-licensed
ARA example and carries its notice. The engine in `engine/` has no third-party code.
