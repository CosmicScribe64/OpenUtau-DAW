#!/usr/bin/env sh
set -eu

entrypoint="bridge/OpenUtau.Vst.EditorHost/EntryPoints.cs"

if grep -Eq 'PlaybackManager\.Inst\.AudioOutput[[:space:]]*=[[:space:]]*new[[:space:]].*MiniAudioOutput' "$entrypoint"; then
  echo "Embedded editor must not open MiniAudio/CoreAudio beside the DAW." >&2
  exit 1
fi

if ! grep -Eq 'PlaybackManager\.Inst\.AudioOutput[[:space:]]*=[[:space:]]*previewAudioOutput' "$entrypoint"; then
  echo "Embedded editor must install the device-free VST preview output." >&2
  exit 1
fi

if grep -Eq 'DllImport|ou_init_audio_device|CoreAudio|PortAudio' \
    bridge/OpenUtau.Vst.EditorHost/VstPreviewAudioOutput.cs; then
  echo "VST preview output must not bind or open a hardware audio API." >&2
  exit 1
fi

echo "Embedded editor audio-device isolation check passed."
