# Public release checklist

Keep the command output or test artifact for each checked item. A passing
container build does not count as a DAW test.

## Source and packages

- [x] A clean source commit and reproducible complete corresponding-source
  archive exist (local public-alpha candidate, 2026-08-19).
- [ ] An annotated version tag and GitHub draft prerelease exist.
- [x] `docker compose run --rm dev ./scripts/ci.sh` passes for the candidate
  commit (2026-08-19).
- [x] macOS native VST3 is rebuilt from the candidate on a Mac with CMake; do not
  use `SKIP_NATIVE_BUILD=1` for a public candidate.
- [x] `scripts/verify-macos-package.sh` passes the clean-rebuilt local archive
  (ad-hoc-signed candidate, 2026-08-19).
- [x] The community macOS alpha's ad-hoc signature, absent Apple notarization,
  checksum-first install path, and targeted Gatekeeper procedure are disclosed.
- [ ] The Windows x64 GitHub package passes its native editor smoke test,
  package verification, and Steinberg validator.
- [ ] The Intel macOS package passes native x64 build, architecture, signature,
  dependency, checksum, and manifest verification on `macos-15-intel`.
- [ ] The Linux x64 package passes native build, loaded-VST3 smoke, dependency,
  checksum, and manifest verification on Ubuntu 24.04.
- [x] Windows is prominently marked experimental until a licensed FL Studio
  acceptance render with retained FLP/reference-WAV assets passes.

## Licensing and notices

- [x] The repository selects and documents JUCE's AGPLv3 open-source path.
- [x] The generated managed-dependency inventory and private .NET runtime
  notices are audited in the clean-rebuilt candidate (75 runtime packages).
- [x] Upstream OpenUtau MIT, JUCE, VST3 SDK, .NET, AGPLv3, and collected
  third-party notices ship with the clean-rebuilt candidate.

## FL Studio and DAW acceptance

- [x] Install the exact packaged macOS archive and reopen it in FL Studio.
- [x] Confirm discovery as the **OpenUtau DAW** VST3 generator and one embedded
  OpenUtau editor window (clean-rebuilt candidate, 2026-08-19).
- [x] Confirm FL transport, tempo, time signature, plain Space, and
  Command/Control editor shortcuts behave as documented.
- [x] Listen to the multilingual multi-singer fixture and confirm the complete
  FL mixer/effect chain and an offline WAV export (exact archive, 2026-08-19).
- [x] Confirm the separately installed OpenUtau desktop app can run alongside
  FL Studio with the VST project loaded (2026-08-19).
- [ ] Repeat on the intended host buffer/sample-rate matrix without changing a
  user’s working configuration unexpectedly.
- [x] Exercise sidecar exit, timeout, and missing-engine recovery in real FL
  Studio with the exact installed candidate (2026-08-19).
- [x] Accept and document one visible editor at a time as a public-alpha known
  limitation; processors render and save independently.
- [ ] Perform acceptance in every additional DAW/platform before claiming it
  supported.

## Publication

- [ ] Publish checksums, package manifest, versioned release notes, licences,
  known limitations, and support/install instructions together.
- [ ] Retain the build and acceptance evidence referenced above.
