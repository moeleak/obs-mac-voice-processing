# AGENTS.md

An OBS Studio audio filter for macOS, built as a plugin bundle: it runs
Apple's voice processing unit on a source's microphone and swaps the source's
audio for what the unit captures (see README.md for how).

## Commits

This repository uses [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>[(<scope>)][!]: <description>

[body]

[footers]
```

- Types: `feat`, `fix`, `perf`, `refactor`, `docs`, `build`, `ci`, `test`,
  `style`, `chore`. Scopes are optional, such as `filter`, `locale`, `nix`.
- The description is lowercase and imperative, without a trailing period, and
  the whole first line stays within 72 characters. The body, wrapped at 72,
  says why.
- A change that breaks scenes users have saved is a breaking change: mark it
  with `!` and a `BREAKING CHANGE:` footer.
- Commits made with an AI tool carry an `Assisted-by:` trailer naming the tool
  and the model, e.g. `Assisted-by: Claude Code (claude-opus-5-5)`.

## Layout

- `src/voice-processing.c`: the filter. `src/plugin-main.c`: the module.
  `src/audio-device-enum.*`: OBS Studio's, from its mac-capture plugin.
- `data/locale/*.ini`: its text; every locale has the same keys as `en-US.ini`.
- `CMakeLists.txt`, `cmake/Info.plist.in`: the plugin bundle.
- `package.nix`: the Nix package. Other flakes call it with `callPackage`
  from this repository's source (moeleak/flakes takes it as a `flake = false`
  input), so it takes nothing but nixpkgs' arguments, and its `src` is this
  directory.
- `flake.nix`: the package, OBS with it (`.#obs`), and an overlay.
- `.github/`: CI, which builds `.#obs` on GitHub's macOS runners and runs
  `.github/scripts/test-voice-processing` against the app, as wrapOBS makes
  it and as Home Manager's copyApps copies it.

## Building and testing

The plugin builds on macOS only, against libobs:

```sh
nix build .#obs   # aarch64-darwin
```

Without a Mac, CI is the build and the test. Before pushing, the sources can
be checked with clang against the macOS SDK and OBS's libobs headers
(`-target arm64-apple-macos13.0 -isysroot <SDK> -fsyntax-only`).

OBS's frontend crashes on GitHub's headless macOS VMs, so the test runs the
filter on a microphone with libobs alone; it checks how often the unit
starts, that the filter logs its buffer, that OBS's CoreAudio source on the
same microphone keeps capturing, and that nothing leaks.

## Style

- C: `clang-format` with `.clang-format` (OBS Studio's).
- Nix: `nixfmt`.

## Keep in mind

- The filter's id (`mac_voice_processing_filter`) and its settings' keys are
  saved in users' scenes: renaming either drops the filter or its settings
  from them.
- Never set `kAudioUnitProperty_MaximumFramesPerSlice` on the unit: it grows
  the microphone's IO buffer for the whole process, and OBS's CoreAudio
  source on the same microphone then fails every render with -50.
- The unit and the source capture on two clocks: the buffer between them is
  read faster or slower to stay level, and what is lost is logged, so a
  change to it should keep `behind 0 times, 0 frames dropped` in CI's log.
