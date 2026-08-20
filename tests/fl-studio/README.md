# FL Studio E2E fixture

`OpenUtauVst.e2e.flp` and `OpenUtauVst.expected.wav` are the binary fixtures
used by the licensed, self-hosted Windows test lane. The FLP must contain one
OpenUtau DAW instance with the repository's approved USTX/singer fixture and a
playlist region that covers the expected vocal output. The WAV must be the
approved stereo render produced by that exact fixture and render settings.

The runner installs the packaged VST3 and invokes FL Studio's documented `/D`,
`/R`, `/Ewav`, and `/O` command-line export options. The resulting WAV is then
checked for existence and compared sample-by-sample with the approved WAV using
the peak and RMS tolerances declared by the runner. It rejects stale or
ambiguous WAV outputs, terminates a render that exceeds its bounded timeout,
and emits `evidence.json` with SHA-256 hashes for FL Studio, the FLP, reference
and actual WAVs, and every packaged-plugin file.

The FLP itself cannot be generated or validated in a Linux container. Do not
claim the FL Studio gate has passed unless both files exist and the self-hosted
job has produced a matching render artifact.
