# Architecture

## Process and thread boundaries

OpenUtau synthesis can invoke resamplers, models, filesystem I/O, managed code,
and garbage collection. Those operations cannot run on a DAW audio callback.
OpenUtau DAW therefore has three cooperating roles:

1. **Native VST3 processor.** FL Studio owns this component. It receives host
   transport and state callbacks, emits stereo audio, and moves only prepared
   samples on the real-time thread.
2. **In-process managed editor.** The VST loads a private .NET runtime through
   `hostfxr`, creates an Avalonia `EmbeddableControlRoot`, and parents its native
   view inside the JUCE/FL plug-in window. It contains the full OpenUtau UI.
3. **Out-of-process managed render engine.** Each processor launches its own
   authenticated loopback sidecar. It owns an isolated USTX document, singer
   lookup, phonemizers, resamplers, and render cache. A stalled renderer cannot
   block the DAW message or audio threads.

Editor state and engine state are serialized as USTX. The native worker talks
to the sidecar over a versioned, token-authenticated loopback TCP protocol.
Render blocks are framed and Base64 encoded on that worker and placed in a
bounded lock-free ring. Shared-memory IPC is a possible optimization, not a
claim about the current implementation.

The audio callback never performs IPC, filesystem access, process management,
managed execution, or a blocking render. Offline export is the deliberate
exception to realtime behavior: the host's non-realtime callback waits for a
bounded deterministic sidecar render.

## DAW contract

The plug-in is a VST3 instrument with stereo output, no audio input, no MIDI
input, and no MIDI output. Vocal notes and lyrics are authored in OpenUtau. The
host supplies sample rate, absolute sample position, play state, offline mode,
quarter-note position, tempo, and time signature.

Host BPM and time-signature changes are reflected in the USTX project without
adding user undo entries. A seek or loop jump changes the render epoch and
invalidates stale queued frames. If matching rendered audio is not ready, the
plug-in emits safe silence; output fades in over 128 samples after an underrun
or discontinuity.

The plug-in reports zero samples of latency. It pre-renders while FL is stopped
instead of asking FL to delay every other mixer path. This avoids the timing
and whole-project distortion observed when FL was forced through an artificial
vocal pre-roll.

## Embedded editor and shortcuts

The editor reuses OpenUtau's original controls and command handlers rather than
implementing a reduced launcher. Moving `MainWindow.Content` under an embedded
root removes the detached `MainWindow` from Avalonia's routed-event ancestry,
so `EmbeddedShortcutRouter` tunnels key events back to the original shortcut
table.

On macOS, a view-local native responder forwards Space and Command+Space (plus
the documented Control+Space spelling) to the FL wrapper. Command shortcuts
are translated for OpenUtau and dispatched back to the embedded Avalonia view.
There is no process-global keyboard event monitor.
The result is intentional ownership: FL handles stop/rewind and pause/resume;
editor commands such as Undo, Redo, Save/Export, Open, selection, and note
editing are handled by OpenUtau. A host stop transition still updates the
OpenUtau playhead, but suppresses automatic scrolling so the editing viewport
does not jump back to the host's restart position.

OpenUtau preview playback is rendered to an in-memory stereo ring and consumed
by the VST callback only while the DAW is stopped. It therefore follows FL's
mixer routing and effects and never opens a second CoreAudio device. Once FL
starts, its transport becomes authoritative and editor preview stops.

Upstream OpenUtau uses singleton editor managers. The renderer and state of
each VST processor are independent, but two simultaneous in-process editor
roots would otherwise address the same `DocManager`. The native runtime leases
the editor to one visible instance and gives a clear message to a second. The
lease is released as soon as the first editor closes. Removing this visible
one-editor limit requires an upstream instance-scoping refactor and remains a
future enhancement. It is an explicit first-public-alpha limitation. This
restriction is confined to embedded VST editor
roots sharing one DAW process. The standalone OpenUtau application runs in its
own process and can remain open alongside FL Studio and the VST3.

## State and saving

The VST state chunk has a magic value, schema version, length, and full UTF-8
USTX payload. Editor commands increment a managed revision. While the editor is
open, the JUCE message thread pulls changed state at 30 Hz; a DAW save callback
also performs a final synchronous pull so the last edit cannot fall into the
timer interval. The processor then notifies the host of non-parameter state
changes.

Saving the FLP is sufficient: the USTX is restored by `setStateInformation`
when FL reloads the plug-in. In plug-in mode, OpenUtau's Save label explains
that state is saved with the DAW. **Export USTX Copy…** writes an optional
external file for interchange or backup. Singer voicebanks, resamplers, and
external models are referenced rather than embedded, so another machine must
install matching dependencies.

## Failure isolation

If a sidecar exits or a request times out, the VST continues returning silence,
reports the error, reconnects on its worker thread, and reapplies cached state.
Render-ahead requests have bounded deadlines; offline synthesis has a longer
bounded deadline. Manual restart exercises the same recovery path. Automated
tests force a process exit, a render hang, and a temporarily missing engine
binary, and prove automatic relaunch without blocking the audio callback.
Managed tests also cover missing-singer persistence and bundled resampler and
wavtool fallbacks. Real FL process injection remains a separate release gate.
