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

The container owns the .NET SDK, Linux C/C++ toolchain, NuGet cache, CMake
downloads, and test fixtures. Do not commit `artifacts/`, `.build/`, voicebanks,
FL projects containing third-party audio, or generated singing output.

A native macOS VST3 must be linked on macOS. `scripts/package-macos.sh` still
uses Docker for managed publishing by default and accepts a temporary CMake
path for the final Apple binary. Host publish mode exists only for ephemeral
GitHub-hosted macOS release runners, where Docker is unavailable.

## Verification expectations

Changes must keep the full container suite green. Audio-thread changes require
tests proving no blocking IPC, filesystem access, process control, or managed
execution occurs in the real-time callback. Host-integration changes should
include deterministic native coverage and retained real-DAW evidence where
automation cannot prove GUI behavior.

Voicebanks and their rendered output are third-party material. Never add them
to the repository or public test artifacts without explicit redistribution
permission.
