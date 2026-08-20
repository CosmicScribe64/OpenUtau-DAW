# Contributing

OpenUtau DAW is an AGPLv3 project built around a pinned MIT-licensed OpenUtau
baseline. Contributions to the VST adapter, bridge, tests, and packaging are
made under the repository's AGPLv3 terms. Changes to the upstream-derived tree
must also remain representable in `patches/openutau-vst.patch`.

## Development environment

Use Docker for the supported development and regression environment:

```sh
docker compose build dev
docker compose run --rm dev ./scripts/ci.sh
```

The Docker image supplies the .NET SDK, Linux C/C++ toolchain, and other build
dependencies. Do not commit `artifacts/`, `.build/`, voicebanks, FL projects
containing third-party audio, or generated singing output.

A native macOS VST3 must be linked on macOS. `scripts/package-macos.sh` uses
Docker for managed publishing and the host CMake executable for the Apple
binary. GitHub's macOS runners use host publish mode because Docker is not
available there.

## Verification expectations

Changes must keep the full container suite green. For audio-thread changes,
add a test that catches blocking IPC, filesystem access, process control, or
managed execution in the real-time callback. For host integration, add native
coverage where possible and note what was checked by hand in a DAW.

Voicebanks and their rendered output are third-party material. Never add them
to the repository or public test artifacts without explicit redistribution
permission.
