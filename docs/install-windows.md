# Install on Windows x64

> **Experimental:** the packaged editor opens in the Windows smoke host and
> the VST3 passes Steinberg's validator. It has not yet been tested in a
> licensed copy of FL Studio on Windows.

1. Download the Windows x64 ZIP and its `.sha256` sidecar from the same GitHub
   release.
2. In PowerShell, verify the download. Replace the filename if necessary:

   ```powershell
   (Get-FileHash -Algorithm SHA256 .\OpenUtau-DAW-*-windows-x64.zip).Hash.ToLowerInvariant()
   Get-Content .\OpenUtau-DAW-*-windows-x64.zip.sha256
   ```

   The two hashes must match.
3. Quit FL Studio.
4. Extract the ZIP and copy `OpenUtau DAW.vst3` to:

   ```text
   C:\Program Files\Common Files\VST3\
   ```

5. Reopen FL Studio, run **Manage plugins → Find installed plugins**, and add
   **OpenUtau DAW** as a generator.

The package carries its own private .NET runtime. Users do not install .NET,
Docker, CMake, OpenUtau, or a development SDK. Voicebanks are not bundled and
must be installed through the embedded OpenUtau editor.

Saving the FLP stores the USTX project in the plug-in state. **Export
USTX Copy…** is optional interchange or backup. Voicebanks, resamplers, and
external models are referenced and must exist on every machine opening the
project.

Multiple processors render and save independently. In this alpha, only one
embedded editor can be visible at a time inside one DAW process; close that
editor before opening another instance.
