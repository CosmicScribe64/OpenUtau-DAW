# GitHub community release

The `Build community release candidate` workflow creates or updates a **draft
prerelease** from an existing annotated version tag. It does not require an
Apple Developer account or repository secrets.

The workflow:

1. runs the canonical Docker regression suite;
2. builds the native Windows x64 VST3 on Windows, opens its embedded editor in
   the smoke host, and runs Steinberg's validator;
3. builds the native Apple-silicon VST3 on an arm64 macOS runner and applies an
   ad-hoc code signature;
4. creates the complete corresponding-source archive containing the pinned
   OpenUtau and JUCE source trees;
5. verifies all three SHA-256 sidecars after transferring the assets between
   isolated jobs; and
6. attaches both platform ZIPs, the source archive, and all checksum sidecars
   to a draft GitHub prerelease.

All third-party GitHub actions are pinned to immutable full commit SHAs. The
jobs install .NET SDK 8.0.424, checkout does not persist the workflow token in
Git configuration, the Steinberg validator source is pinned to an exact Git
commit, and only the final draft-release job receives `contents: write`
permission.

## Create a release candidate

Create and push an annotated tag pointing at the reviewed clean commit:

```sh
git tag -a v0.1.0-alpha.1 -m "OpenUtau DAW v0.1.0-alpha.1"
git push origin main
git push origin v0.1.0-alpha.1
```

The tag push starts the workflow automatically. A manual rerun is also
available in Actions, but **Use workflow from** must select that exact tag and
the input must contain the same tag. The workflow rejects branch refs,
lightweight tags, and tags resolving to a different workflow commit.

Signing the Git tag remains encouraged when a GitHub-recognized signing key is
available, but is not required for this community alpha. SSH authentication,
the exact tag/commit gate, immutable action pins, package manifests,
corresponding source, and retained Actions logs provide the available
provenance.

## Review and publish

The workflow deliberately stops at a draft. Before selecting **Publish
release** in GitHub, confirm:

- all four workflow jobs passed;
- the draft contains macOS arm64, Windows x64, source, and three checksum
  sidecars;
- Windows is labeled experimental in the notes;
- macOS is labeled ad-hoc signed and not Apple-notarized;
- the checksum sidecars match their downloads; and
- the known one-visible-editor limitation remains disclosed.

GitHub-hosted macOS runners are used in host mode because Docker is unavailable
there. Normal development, managed publishing on the target Mac, and the
canonical portable regression suite remain Docker-first.

If the project later obtains Apple Developer credentials, the same macOS
package script supports Developer ID signing, notarization, and stapling with
`CODESIGN_IDENTITY`, `NOTARIZE=1`, and the `NOTARY_*` inputs. Those credentials
must be kept in encrypted Actions secrets and never committed.
