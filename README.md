# OpenUtau DAW

OpenUtau DAW runs the OpenUtau editor as a VST3 instrument. Write notes,
lyrics, pitch, and singer changes in OpenUtau while the DAW handles playback,
tempo, mixing, and project files.

![OpenUtau DAW open inside FL Studio, with the transport, Channel Rack, Playlist, Mixer, vocal arrangement, and pitch editor visible](docs/images/openutau-daw-fl-studio-multisinger.png)

The current release is an early alpha. It has been used in FL Studio 2026 on
an Apple-silicon Mac; the other builds have automated test coverage but still
need more time in their target DAWs.

## Download

Packages and checksums are on the
[releases page](https://github.com/CosmicScribe64/OpenUtau-DAW/releases).
Voicebanks are not included.

| Build | Status |
| --- | --- |
| macOS arm64 | Tested in FL Studio 2026; ad-hoc signed and not notarized |
| macOS Intel | Builds and packages successfully; DAW testing pending |
| Windows x64 | Editor, audio, and VST3 validation tests pass; FL Studio testing pending |
| Linux x64 | Experimental; opens the editor in a separate window |

Installation notes:

- [macOS](docs/install-macos.md)
- [Windows](docs/install-windows.md)
- [Linux](docs/install-linux.md)

## What works

- The editor follows the DAW transport, tempo, and time signature.
- Preview and rendered audio pass through the plug-in output.
- OpenUtau project data is saved in the VST state, so it travels with the DAW
  project.
- Multiple instances keep separate audio and project state.
- Closing an editor, or moving between instances, returns to that instance's
  previous piano-roll view.
- Common transport shortcuts are passed back to FL Studio. Editor shortcuts
  such as undo, redo, delete, and open stay in OpenUtau.

## Limitations

- Notes must be entered in OpenUtau; the plug-in does not accept MIDI notes
  from the DAW.
- Only one embedded editor can be open at a time. Audio and saved state still
  work independently in every instance.
- The macOS package is not notarized.
- Windows, Intel macOS, Linux, and DAWs other than FL Studio have not had full
  hands-on testing yet.

Please include the operating system, DAW version, and plug-in build when
reporting a problem.

## Building and testing

The normal development environment is Docker Compose:

```sh
docker compose build dev
docker compose run --rm dev ./scripts/ci.sh
```

For a quicker editor test after building the image:

```sh
docker compose run --rm dev dotnet run --project \
  bridge/OpenUtau.Vst.EditorHost.Tests --configuration Release
```

The native macOS binary must be linked and signed on macOS. The packaging
script uses Docker for the managed components and the host toolchain for that
final step. See the [release checklist](docs/release-checklist.md) for the full
process.

OpenUtau is pinned as a submodule at
`29e0e16d1623cda79ba7c3724614d6129ba3b9d5`. The VST-specific changes live in
`patches/openutau-vst.patch`; the build scripts apply that patch before
compiling.

More detail is available in the [architecture notes](docs/architecture.md) and
[test record](docs/verification.md).

## Licence

OpenUtau's source is MIT licensed. The plug-in and combined distribution use
JUCE under the AGPLv3. Release archives include the relevant licence notices
and corresponding source; see [docs/licensing.md](docs/licensing.md).
