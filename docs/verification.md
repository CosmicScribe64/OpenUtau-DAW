# Verification and release gates

Verification is split between reproducible container tests and acceptance in
the real DAW. Passing Linux/container tests is not, by itself, proof that a GUI
host integration works.

## Automated suite

The canonical command is:

```sh
docker compose run --rm dev ./scripts/ci.sh
```

On 2026-08-19 the complete suite passed after the embedded-shortcut,
no-MIDI-input, stop-viewport, restore-time phonemizer-context, package-manifest,
and runtime-notice changes:

- pinned OpenUtau patch replay against a clean baseline;
- hardware-audio isolation for the embedded editor;
- managed build, protocol round-trip, USTX state, and host timing tests;
- embedded descendant-to-root shortcut routing and device-free preview tests;
- Windows publish-content inspection;
- native audio ring, sidecar client, transport epoch, and offline-render tests;
- automatic recovery after a forced sidecar exit, a bounded render hang, and a
  missing engine binary that is restored while the plug-in remains loaded;
- safe missing-singer state persistence plus audible bundled fallbacks when a
  saved classic resampler or wavtool is unavailable;
- VST3 discovery/instantiation, safe disconnected silence, and zero-latency
  contract;
- generated classic voicebank phonemization/synthesis at 44.1/48/96 kHz;
- an actually loaded VST3 producing audible real-time and offline singing;
- the loaded VST3 at every 44.1/48/96 kHz × 64/128/256/512/1024/2048-frame
  host-callback combination, compared against a direct sidecar reference.

CTest result: **6/6 passed, 0 failed**.

## Current macOS artifact

`artifacts/OpenUtau-DAW-macos-arm64.zip` is an arm64 release-candidate package
whose VST3 passes strict deep code-signature verification with its development
ad-hoc signature. On 2026-08-19 the native module was rebuilt from the current
source in a new CMake build directory using AppleClang and pinned JUCE 8.0.15;
managed components and the private runtime were republished through Docker.
The exact archive SHA-256 is recorded in its sibling `.zip.sha256` file rather
than embedded here, which avoids a self-referential package checksum. The
native module SHA-256 is
`56404c9f0afb02948a584ce1c36b4b1bc08b2d957fbd0542aa6d8a44a736145b`.

`scripts/verify-macos-package.sh` is the reproducible workspace-only package
gate: it validates the portable archive checksum, internal manifest, strict
bundle signature, arm64 module/Worldline architectures, lack of Finder metadata
in the ZIP, and lack of user-local/Homebrew dependencies in those binaries.

After that clean rebuild, the native smoke host loaded the exact package module
from `artifacts/macos-arm64-release-candidate`, discovered one stereo instrument named
`OpenUtau DAW`, instantiated it at 48 kHz/512 frames, confirmed zero reported
latency and safe disconnected silence, and round-tripped its state chunk. This
is native VST3-host evidence in addition to the real-FL smoke below.

On 2026-08-19, with FL Studio closed, the prior installed bundle was preserved
under `artifacts/installed-backups` and this exact packaged VST3 was installed.
The source and installed bundle compared byte-for-byte, including an identical
native-module SHA-256, and the installed copy passed strict deep signature
verification. FL Studio then reopened the existing two-instance acceptance
project without a missing-plug-in or recovery dialog. The first embedded editor
restored all three singer tracks; Space advanced both FL and OpenUtau transport,
then stopped/rewound it. A full-project pass showed both OpenUtau generators,
Sytrus, drums, and normal FL activity. The fresh log contained no sidecar,
render, phonemizer, timeout, or audio-buffer failure. The user then confirmed
that the exact-archive FL playback sounded good.

The current archive and every newly created distribution archive include `PACKAGE-MANIFEST.sha256`, a
sorted SHA-256 inventory of every distributed file, and carries a sibling
`*.zip.sha256` archive checksum. The packaging command verifies the internal
manifest after creating the ZIP; Docker CI tests both successful verification
and tamper detection. This is package-handoff evidence, not a substitute for
Developer ID signing and notarization.

The same archive now includes the AGPLv3 terms, upstream OpenUtau MIT notice,
JUCE and VST3 SDK notices, private .NET runtime licence and notices, and a
RID-specific inventory of 75 runtime NuGet packages. Fifty-six licence or
third-party-notice files present in those packages are collected into the
archive. The generator rejects missing licence metadata unless an exact package
version has a reviewed, checksum-pinned source override; the current overrides
cover AsyncIO 0.1.69 and Ignore 0.1.50.

## FL Studio acceptance observed on the target Mac

The target environment is Apple-silicon macOS with FL Studio 2026. These items
have been exercised in the real application with computer-use observation and
user listening confirmation:

| Behavior | Evidence status |
| --- | --- |
| Exact verified archive installed byte-for-byte and reopened in FL | Passed on 2026-08-19 |
| Clean-rebuilt native module installed byte-for-byte and loaded in FL | Passed on 2026-08-19; module SHA-256 `56404c9f…6145b` |
| Clean discovery as `OpenUtau DAW` VST3 generator | Passed |
| Full OpenUtau workspace embedded in one FL plug-in window | Passed |
| Audible Teto phrase synchronized to FL transport | Passed |
| FL BPM and play position shown in OpenUtau | Passed |
| Plain Space from the editor starts/stops FL | Passed |
| Command+Space pauses FL and stop/rewind retains the editor viewport | Passed in the installed macOS VST3 inside FL Studio 2026 |
| Command/Control Undo stays in OpenUtau | Passed by user confirmation |
| Other FL instruments remain correct at the shared audio rate | Passed by user listening confirmation |
| VST output reaches an FL mixer insert and Fruity Reeverb 2 | Passed by user listening confirmation |
| Two processor instances coexist and render/store state independently | Exercised; only one full editor may be visible |
| Installed standalone OpenUtau coexists with FL and the loaded VST project | Passed on 2026-08-19; both processes remained alive |
| Multiple singers/languages/notes in the acceptance USTX | Exact-archive FL playback confirmed good by the user on 2026-08-19 |
| Save FLP, close FL, reopen, and prove the complete OpenUtau state restored | Passed across a fresh FL Studio process |
| Offline FL WAV export and reference-render comparison | Passed with the exact installed archive on 2026-08-19 |
| 44.1/48/96 kHz and 64–2048 sample host-buffer matrix | Loaded-VST3 container matrix passed; real-FL hardware matrix still required |
| Forced sidecar crash, hang, missing engine, singer, resampler/wavtool recovery | Passed in container; real-FL process injection remains a release gate |

The clean-rebuilt candidate was installed after preserving the prior bundle.
FL reopened `OpenUtau VST E2E_2.flp`, restored Teto Japanese, Teto English, and
Adachi Rei, embedded the full editor, and accepted Space from inside the editor.
The synchronized OpenUtau playhead reached 5.02 seconds during real playback.
The fresh log contained no sidecar, synthesis, phonemizer, timeout, or audio
buffer failure. This pass did not alter FL's audio device or buffer settings.

## Log audit

The final macOS/FL reload produced no unhandled exceptions, phonemizer setup
failures, render failures, transport timeouts, or audio under/overflows. The
remaining diagnostics were reviewed and are not VST failures:

- Adachi Rei 3.5.0's bundled sample-only `oto.ini` references two older WAV
  names that are absent from the supplied archive. The installed archive has
  newer sample WAV names; singing aliases used by the project still load.
- The hand-edited demo FLP contains the lyric `- me`, whose standalone hyphen
  is not in the English dictionary. The real phonemes still resolve; release
  fixtures use deterministic `hi` lyrics and do not rely on that entry.
- `Part rendered. not on main thread` is OpenUtau's diagnostic before
  `DocManager` posts the notification onto its UI scheduler. Render completion
  and waveforms were observed immediately afterward.

The historical literal `error` phonemes were a restore-time context race:
queued phonemizer work indexed the process-global current project instead of
the project and track that created the request. The request now captures both
objects; a focused regression test covers an invalid/stale `part.trackNo`, and
a fresh FL reload showed real English phonemes with no recurrence in the log.

## Fault-recovery evidence

The native bridge test now launches a purpose-built, test-only protocol peer
and exercises the same process/client loop used by the VST3. It proves all of
the following without modifying the host machine or a user's voicebanks:

- a sidecar that exits during its first render is detected, disconnected,
  relaunched, and resumes full audio blocks;
- a sidecar that hangs past the configured request deadline is killed,
  relaunched, and resumes full audio blocks;
- an absent engine executable produces silence and a useful error instead of
  blocking the audio callback, then connects and renders after the executable
  appears at the expected path;
- a missing singer reloads as a stable placeholder, renders deterministic
  finite silence, and retains its identifier through another DAW-state save;
- missing saved classic resampler/wavtool names resolve to the bundled
  Worldline and SharpWavtool implementations and still render audible finite
  audio.

These are deterministic container tests, not a claim that process injection
has been performed inside the user's live FL Studio session. That final host
exercise remains intentionally separate.

The working FL project is
`~/Documents/Image-Line/FL Studio/Projects/OpenUtau VST E2E/OpenUtau VST E2E.flp`.
The multilingual fixture is
`artifacts/acceptance-song/multilingual-ensemble.ustx` and contains Teto
Japanese, Teto English, and Adachi Rei tracks. Voicebank-specific buzz on
particular notes/lyrics was separately identified as source-bank behavior, not
whole-host sample-rate corruption.

## Final FL offline-render acceptance

The final installed macOS bundle was rendered twice from FL Studio 2026 using
Full song, stereo WAV, HQ plug-in processing, insert effects, and master
effects. The files are local acceptance evidence rather than redistributable
fixtures because they contain third-party voicebank output:

- `OpenUtau Final Acceptance 20260818.wav`
- `OpenUtau Final Acceptance 20260818 Repeat.wav`

Both renders are 16-bit stereo PCM at 48 kHz, contain 336,214 frames, and are
7.004458 seconds long. The first render measured -1.13 dBFS peak and -17.33
dBFS RMS, contained no clipped samples, and had audible-range signal from the
first 50 ms window through 6.85 seconds. Its decoded-PCM SHA-256 is
`7c72c266f2f1682e1f51acdaa49540dcfd359b2a50957f91944d3677960bb7e6`.

The independent repeat render had the same layout and exact frame count. Its
sample correlation with the first was 0.9999957754; the null difference was
12.95 16-bit LSB RMS (-68.07 dBFS) with a 384-LSB peak. Low-level variation is
expected from the enabled time-based mixer effects. No OpenUtau exception,
phonemizer failure, render failure, timeout, or audio-buffer diagnostic was
logged during either export.

## Exact-archive FL offline-render smoke

After installing the byte-identical packaged bundle on 2026-08-19, FL Studio
rendered `artifacts/fl-studio-exact-archive-smoke/OpenUtau Exact Archive FL
Smoke 20260819.wav` from the two-instance acceptance project. The export used
Full song, stereo 16-bit WAV at 48 kHz, HQ plug-in processing, insert effects,
master effects, and the reverb tail. It contains 336,214 frames (7.004458
seconds), measured -1.13 dBFS peak and -17.33 dBFS RMS, had no clipped samples,
and was active in 138/141 50-ms windows from 0.000 through 6.900 seconds. Its
SHA-256 is
`9cb3cc5d3dc39b0a1ba9bd312511a13bf735215bcaca026ff845d93b0cb6ee4d`.

The exact-archive render and the prior accepted render have identical layout
and frame count, correlation 0.9999901759, and a -64.40 dBFS null RMS with a
394-LSB peak. That low-level variation is consistent with the enabled
time-based effects. The post-render log contained no sidecar, render,
phonemizer, timeout, or audio-buffer failure.

## Public-release blockers

The current result is a local public-alpha release candidate. Do not publish
the binary until the remaining applicable items below are closed:

The executable handoff checklist is [release-checklist.md](release-checklist.md).

1. Complete the remaining real-host buffer matrix without unexpectedly
   changing a user's working audio configuration.
2. Sign with an appropriate Apple Developer ID, notarize, staple, and test on a
   clean Mac. The current local package is ad-hoc signed only.
3. Complete crash/hang/missing-dependency recovery in real FL Studio.
4. Create a clean, versioned source release and run the signed draft-release
   workflow with repository signing/notarization secrets;
   the working root repository has not yet been committed/tagged.
5. For Windows support, produce and sign the x64 package, run Steinberg's
   validator, and complete the licensed FL Studio runner with checked-in
   FLP/reference-WAV fixtures. Until then, Windows is not a verified target.
6. Acceptance-test every additional DAW/platform before listing it as
   supported.

The one-visible-editor constraint is accepted as a clearly documented
limitation for the first public alpha; processors remain independent. The
AGPLv3 distribution path and exact managed dependency notice inventory are now
implemented. A later release may refactor upstream singleton editor services.

## Windows licensed-host lane

`scripts/package-windows.ps1` and the `windows-vst3` workflow define the
self-contained package and validator work. `tests/fl-studio` defines a bounded,
stale-output-safe FL command-line render with hashed evidence. The binary FLP
and reference WAV cannot be produced faithfully in Linux and are still absent.
No Windows compatibility claim should be made until that lane produces its
artifacts on a licensed runner.
