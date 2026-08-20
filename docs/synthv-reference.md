# FL Studio interaction notes

Synthesizer V Studio 2 was used as a reference for how a singing editor can
behave inside FL Studio. These notes describe the interaction, not its artwork
or implementation.

## Reference behavior

The VST3 appears in FL Studio's generator list and opens its normal editing
workspace in the plug-in window. Notes are written in that workspace rather
than in FL Studio's piano roll. Dreamtonics' FL Studio Link follows playback
and tempo when FL Studio is in Song mode; its documentation says time
signatures are not synchronized.

References:

- [Plugins and DAW Integration](https://sv2.docs.dreamtonics.com/en/plugins)
- [Interface](https://sv2.docs.dreamtonics.com/en/interface)
- [FL Studio compatibility announcement](https://dreamtonics.com/announcing-synthesizer-v-studio-2-pro/)

## Decisions for OpenUtau DAW

OpenUtau DAW follows the same broad division of work:

- FL Studio owns transport, tempo, mixing, effects, and export.
- OpenUtau owns singers, lyrics, phonemes, notes, and pitch editing.
- The OpenUtau workspace opens inside the VST window on macOS and Windows.
- Project state is stored in the FLP through the VST state chunk.
- Closing the editor does not stop the processor or discard edits.

Unlike the reference plug-in, OpenUtau DAW also follows the host time
signature. It does not copy Dreamtonics branding, visual design, or proprietary
behavior.
