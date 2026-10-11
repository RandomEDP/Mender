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

- **Open** a WAV, AIFF or FLAC, or drop one on the window.
- **Drag** a box around the noise. Keep it tight: anything inside the box that
  swells louder than the audio at its edges is pulled down too.
- **Repair** (Enter or R). **Undo/Redo** with Ctrl+Z / Ctrl+Y.
- **Space** plays from one second before the box. **Hear original** switches
  playback to the untouched clip for comparison.
- Mouse wheel zooms in time, Shift + wheel scrolls, double-click zooms to the
  box, Esc clears it.
- **Save As** writes a new WAV at the source sample rate, 24-bit or better.

Controls:

- **Fill from**: *Before and after* suits short noises over steady sound.
  *Above and below* suits longer noises in a narrow band.
- **Resolution**: more bands separate close pitches; fewer follow fast sounds.
- **Tolerance**: lower removes more, higher protects the wanted sound.

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
