# Test record

The project has automated tests in Docker and a smaller set of hands-on checks
in FL Studio. A passing container run does not count as DAW testing.

## Automated tests

Run the suite with:

```sh
docker compose run --rm dev ./scripts/ci.sh
```

The suite last passed on 2026-08-19, including all six CTest cases. It checks:

- replay of the VST adapter patch against the pinned OpenUtau commit;
- protocol and project-state round trips;
- shortcut routing and preview-audio isolation;
- the audio ring and render sidecar;
- recovery after a crash, timeout, or missing engine executable;
- singer-state persistence and fallback rendering tools;
- VST3 discovery, loading, disconnected silence, and latency reporting;
- live and offline singing at 44.1, 48, and 96 kHz with several host block
  sizes.

The singing fixture uses a generated classic voicebank. Tests compare VST
output with a direct render from the same engine, so no third-party voicebank
needs to be distributed.

## macOS arm64 package

The alpha package was rebuilt on 2026-08-19 with AppleClang and JUCE 8.0.15.
It is ad-hoc signed and not notarized.

To check a package:

```sh
scripts/verify-macos-package.sh \
  artifacts/OpenUtau-DAW-v0.1.0-alpha.2-macos-arm64.zip
```

The script verifies the archive checksum, manifest, bundle signature,
architectures, library paths, and absence of Finder metadata. The package
contains the AGPL, OpenUtau, JUCE, VST3 SDK, and .NET notices plus a generated
NuGet dependency inventory.

## FL Studio on macOS

The packaged arm64 build was installed and checked in FL Studio 2026 on
2026-08-19. The test covered:

- plug-in discovery and editor opening;
- transport, tempo, time signature, and keyboard shortcuts;
- preview and singing audio through an FL mixer insert;
- FLP save, quit, reopen, and project-state restoration;
- two processor instances with one editor visible at a time;
- offline WAV export;
- recovery after a sidecar exit, timeout, and missing engine-host DLL.

The project used Teto Japanese, Teto English, and Adachi Rei. Those voicebanks
and the rendered WAVs remain local because they are third-party material.

## Not yet covered

- FL Studio on Windows
- a real Intel Mac
- a native Linux DAW
- DAWs other than FL Studio
- the full sample-rate and buffer-size matrix in FL Studio
- Developer ID signing and notarization

Until those checks are done, their builds should remain marked experimental.
The Windows fixture is described in
[`tests/fl-studio`](../tests/fl-studio/README.md).
