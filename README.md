# obs-mac-voice-processing

An OBS Studio audio filter for macOS that replaces a microphone source's audio
with the microphone as Apple's voice processing
(`kAudioUnitSubType_VoiceProcessingIO`) captures it, as calls on macOS have it:

- without the echo of what the output device plays, whichever application
  plays it, so that a call on speakers can be recorded without the other side
  being heard twice;
- with noise suppressed;
- optionally, with automatic gain control.

Add **Voice Processing (macOS)** to a microphone source's filters. Its
properties choose the microphone (the source's own by default), the output
device whose echo is cancelled, the gain control, how much macOS lowers other
audio meanwhile (macOS 14 and later), and a bypass, to hear the difference.

## How it works

The voice processing unit is an I/O unit: it captures a microphone itself, and
cannot process audio it is handed. The filter runs one on the microphone while
it is enabled and on a source, and swaps the source's frames for the unit's,
through a buffer. The two capture on two clocks, which drift apart, so the
buffer is read up to 0.2% faster or slower, interpolated, to stay at a
fiftieth of a second; should the unit still fall behind, the buffer grows and
what is missing fades out and back in rather than breaking off. How it goes is
logged ten seconds in, and then every ten seconds that anything was lost.

## Building

With Nix, on aarch64-darwin:

```sh
nix build .#obs-mac-voice-processing   # the plugin, in lib/obs-plugins
nix build .#obs                        # OBS with it
```

The flake's `overlays.default` adds it as `obs-studio-plugins.obs-mac-voice-processing`.

Otherwise, with CMake against an OBS Studio build's libobs:

```sh
cmake -B build -Dlibobs_DIR=<OBS>/Frameworks/libobs.framework/Resources/cmake
cmake --build build
```

and copy `build/obs-mac-voice-processing.plugin` into
`~/Library/Application Support/obs-studio/plugins`.

## License

GPL-2.0-or-later, as OBS Studio. `src/audio-device-enum.*` are OBS Studio's,
from its mac-capture plugin.
