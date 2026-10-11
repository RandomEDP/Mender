# Mender (working title)

A spectral repair editor: open a clip, drag a box around an unwanted sound on
the spectrogram, press Repair, save the result as a new file.

Written from scratch from published signal-processing techniques. It contains
no code from any other product.

## What is here

| Folder | Contents |
|---|---|
| `engine/` | The repair algorithm in plain C++17 with no dependencies |
| `tests/` | Engine tests (`ctest`) |
| `app/` | Standalone editor built with JUCE 9 |
| `.github/workflows/` | Builds Windows and macOS downloads on every push |

## Using it

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
macOS). JUCE is downloaded automatically.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
    ctest --test-dir build -C Release

The app is written to `build/Mender_artefacts/Release/`.

## Roadmap

1. Standalone editor (this version).
2. VST3 + ARA2 plug-in so a clip selected in Pyramix opens here directly and
   the repair lands back on the timeline.
3. Better repairs where the noise overlaps wanted partials, harmonic selection,
   click detection.

## Licensing notes

JUCE is dual-licensed: AGPLv3 or a commercial JUCE licence. Before selling or
sharing builds, check the current JUCE terms at https://juce.com and choose one.
The engine in `engine/` has no third-party code.
