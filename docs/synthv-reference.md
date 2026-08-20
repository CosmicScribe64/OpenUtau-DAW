# Synthesizer V / FL Studio reference

This document defines the interaction reference requested for OpenUtau DAW. It
is a behavior reference, not permission to copy Dreamtonics artwork, branding,
or proprietary implementation details.

## Observed on the target Mac

On 2026-08-18, Synthesizer V Studio 2 Plugin (VST3, Apple + Intel) was loaded
from FL Studio 2026's **Add > More plugins** generator list. FL hosted a single
resizable plugin editor containing the complete Synthesizer V workspace. No
launcher panel or separately managed editor window was involved.

The visible plugin surface included:

- application menus and editor transport;
- an arrangement/timeline with tracks, tempo, and time signature lanes;
- the note piano roll and phoneme-timing lane;
- direct note, pitch, waveform, retake, grid, key, and scale controls;
- contextual notes, language, and phoneme controls;
- sidebar entry points for voice, notes, dictionary, effects, rendering,
  scripts, products, and settings;
- an illuminated **FL Studio Link** indicator.

The FL accessibility hierarchy reported this as one
`Synthesizer V Studio 2 Plugin` container within the FL plugin window. This is
the critical distinction from a companion application that merely happens to
be launched by a plugin.

## Vendor-documented behavior

Dreamtonics documents the VST3 as an instrument that appears in the DAW's
instrument list. Notes are authored inside the plugin editor rather than the
DAW piano roll. The version 2 plugin uses a UI consistent with the standalone
editor and exposes the arrangement, piano roll, transport, and side panels in
that hosted surface.

For FL Studio specifically, Dreamtonics documents **FL Studio Link**: the
instrument plugin automatically detects FL Studio and links tempo and playback
without an ARA plugin. FL must be in Song mode. Dreamtonics notes that this
link does not synchronize time signatures.

Primary references:

- [Plugins and DAW Integration](https://sv2.docs.dreamtonics.com/en/plugins)
- [Interface](https://sv2.docs.dreamtonics.com/en/interface)
- [Version 1 plugin screenshot in a DAW](https://sv1.docs.dreamtonics.com/svstudio-user-manual/plugins/plugin_1.png)
- [Dreamtonics announcement listing FL Studio compatibility](https://dreamtonics.com/announcing-synthesizer-v-studio-2-pro/)

## OpenUtau DAW acceptance contract

The OpenUtau implementation is not complete until the following workflow is
demonstrated in the real FL Studio host:

1. **OpenUtau DAW** appears in FL Studio's generator list as a VST3 synth.
2. Opening its channel shows the full OpenUtau editing workspace inside one
   resizable FL plugin window. A small bridge/launcher screen is not accepted.
3. The hosted workspace supports OpenUtau track management, singer and
   phonemizer selection, note and lyric editing, pitch/parameter editing,
   render status, and the project operations available in the source editor.
4. Notes are authored inside the hosted OpenUtau editor. The plug-in does not
   advertise DAW MIDI input; FL's piano roll is not a replacement for the
   singer, lyric, phoneme, pitch, and parameter workflow.
5. FL transport position, play/stop, tempo, and offline bounce drive the same
   project displayed in the plugin. Time signatures should synchronize too,
   since the existing OpenUtau bridge already receives them.
6. USTX state is stored in the FLP/VST state chunk and survives close/reopen;
   explicit USTX import/export remains available for interchange.
7. Closing the plugin window does not lose edits or stop the engine. Reopening
   it restores the same editor state without launching an unrelated desktop
   window.
8. Computer-use E2E must create or load a singing phrase in the hosted editor,
   play it through FL, save/reopen the FLP, and perform an audible offline WAV
   render.

The standalone OpenUtau application may remain as an optional fallback, but it
cannot be the primary editing experience or the evidence used to satisfy the
embedded-editor gate.
