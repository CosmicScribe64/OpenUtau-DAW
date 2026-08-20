# Install on Apple-silicon macOS

1. Download the macOS arm64 ZIP and its `.sha256` sidecar from the same GitHub
   release.
2. Verify the checksum:

   ```sh
   shasum -a 256 -c OpenUtau-DAW-*-macos-arm64.zip.sha256
   ```

3. Quit FL Studio.
4. Extract the ZIP and copy `OpenUtau DAW.vst3` to
   `~/Library/Audio/Plug-Ins/VST3/`.
5. Reopen FL Studio. Rescan installed VST3 plug-ins if **OpenUtau DAW** is not
   yet listed, then add it as a generator.

This community alpha is ad-hoc signed and is not Apple-notarized. If Gatekeeper
quarantines the verified download and FL Studio cannot scan it, quit FL Studio
and remove quarantine from this bundle only:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/OpenUtau DAW.vst3"
```

Do not disable Gatekeeper globally. Future releases may become Developer ID
signed if a project maintainer or sponsor provides that service.

The plug-in carries its own private .NET runtime. Users do not install .NET,
Docker, CMake, OpenUtau, or a development SDK. Voicebanks are not bundled and
must be installed through OpenUtau separately.

Saving the FLP stores the complete USTX project in the plug-in state. **Export
USTX Copy…** is optional interchange or backup. Voicebanks, resamplers, and
external models are referenced and must also exist on another machine opening
the project.

Multiple OpenUtau DAW processors render and save independently. In the public
alpha, only one embedded VST editor can be visible at a time inside a DAW
process; close that editor before opening another instance. The standalone
OpenUtau application may run alongside FL Studio because it is a separate
process.
