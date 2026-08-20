# Release checklist

Keep the test output and package manifests with the release work. Container
tests do not replace opening the plug-in in a DAW.

## Before tagging

- [ ] The worktree and all submodules are clean.
- [ ] `docker compose run --rm dev ./scripts/ci.sh` passes.
- [ ] Release notes match the packages being published.
- [ ] The version is consistent in package names, manifests, and notes.
- [ ] The corresponding-source archive contains the pinned OpenUtau and JUCE
  trees.
- [ ] Licence files and third-party notices are present in every package.

## Packages

- [ ] macOS arm64 is rebuilt on a Mac and
  `scripts/verify-macos-package.sh` passes.
- [ ] macOS Intel has the expected architecture, signature, dependencies,
  checksum, and manifest.
- [ ] The Windows editor smoke test and Steinberg validator pass.
- [ ] The Linux package loads in the smoke host and passes its dependency and
  manifest checks.
- [ ] Each archive has a matching SHA-256 file.

The macOS community build is ad-hoc signed and not notarized. Do not describe
Windows, Intel macOS, Linux, or another DAW as tested until someone has run the
published package there.

## DAW check

- [ ] Install the packaged build, not a development bundle.
- [ ] Confirm discovery, editor opening, transport, tempo, and time signature.
- [ ] Check editor shortcuts and DAW transport shortcuts.
- [ ] Listen through the DAW mixer and export a WAV.
- [ ] Save, quit, reopen, and verify that the project state returns.
- [ ] Check more than one processor instance and the one-visible-editor limit.
- [ ] Exercise sidecar recovery.
- [ ] Record the operating system, DAW version, sample rate, and buffer size.

## Publishing

- [ ] Create an annotated tag from the reviewed commit.
- [ ] Review the draft release and download each uploaded asset.
- [ ] Verify checksums from the downloaded files.
- [ ] Publish packages, source, checksums, release notes, and installation
  instructions together.
- [ ] Keep the build and DAW-test evidence.
