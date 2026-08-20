# Distribution licensing

OpenUtau's upstream source is MIT licensed. The native plugin shell pins JUCE
8.0.15, whose modules are dual-licensed under AGPLv3 or a commercial JUCE
licence. This repository selects the open-source **AGPLv3** path for the VST
adapter and combined binary; the root `LICENSE` contains the licence terms.
The macOS community release is ad-hoc signed and not Apple-notarized. Code
signing and notarization do not change the source licences.

Every binary release must include or accompany the exact corresponding source,
including the pinned OpenUtau baseline, adapter patch, native and managed
bridge source, build scripts, and package scripts. GitHub's automatic source
archive does not include submodule contents, so a recursively populated source
archive must be attached to the release. A distributor choosing commercial
JUCE terms instead must replace this policy and hold an applicable licence.

The macOS and Windows packaging scripts copy the AGPLv3 terms, pinned JUCE
notice, embedded VST3 SDK MIT notice, upstream OpenUtau MIT licence, source
instructions, and verification status beside the bundle. They also generate a
RID-specific inventory from the exact managed publish manifests, copy licence
and third-party-notice files found in runtime NuGet packages, ship the private
.NET runtime's own notices, and reject runtime packages with neither declared
licence metadata nor an exact-version reviewed override.

Before publishing, inspect the finished archive and attach its source archive
to the same release.
