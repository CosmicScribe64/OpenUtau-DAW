# Install on Linux x64

The Linux build is an experimental VST3 package for native Linux DAWs. FL
Studio does not provide a native Linux host, so this artifact is not an FL
Studio build and has not been tested through Wine.

1. Download the `linux-x64.zip` archive and matching `.sha256` file.
2. Verify it with `sha256sum -c OpenUtau-DAW-*-linux-x64.zip.sha256`.
3. Extract the archive.
4. Copy `OpenUtau DAW.vst3` to `~/.vst3/`.
5. Rescan VST3 plug-ins in the DAW and insert **OpenUtau DAW** as an instrument.

The package includes its own .NET runtime. A desktop Linux installation still
needs the normal X11, fontconfig, FreeType, ALSA, and OpenGL libraries used by
JUCE and Avalonia. On Ubuntu these are normally already present on an audio
workstation.

The first Linux alpha uses a companion OpenUtau window launched by the button
in the plug-in's small control panel. The full in-window editor embedding used
on macOS and Windows is not implemented for Linux yet. Save in the companion
editor so the temporary USTX is synchronized back into the plug-in state.
