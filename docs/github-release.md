# GitHub community release

The `Build community release candidate` workflow creates or updates a **draft
prerelease** from an existing annotated version tag. It does not require an
Apple Developer account or repository secrets.

The workflow:

1. runs the Docker regression suite;
2. builds the native Windows x64 VST3 on Windows, opens its embedded editor in
   the smoke host, and runs Steinberg's validator;
3. builds native Apple-silicon and Intel VST3 packages on architecture-matched
   macOS runners and applies ad-hoc code signatures;
4. builds and smoke-tests a Linux x64 VST3 with a companion editor window;
5. creates the complete corresponding-source archive containing the pinned
   OpenUtau and JUCE source trees;
6. verifies all five SHA-256 sidecars after transferring the assets between
   isolated jobs; and
7. attaches four platform ZIPs, the source archive, and all checksum sidecars
   to a draft GitHub prerelease.

All third-party GitHub actions are pinned to immutable full commit SHAs. The
jobs install .NET SDK 8.0.424, checkout does not persist the workflow token in
Git configuration, the Steinberg validator source is pinned to an exact Git
commit, and only the final draft-release job receives `contents: write`
permission.

## Quota-conscious validation

Ordinary feature-branch pushes do not start the hosted matrix. Run the Docker
suite locally while iterating, then use **Build and validate → Run workflow**
only when a platform runner is needed:

- `container` runs only the portable Docker gate;
- `windows-editor` builds the Windows package and exercises its embedded HWND;
- `fl-studio-e2e` targets an explicitly configured licensed self-hosted runner;
- `all` runs the complete cross-platform matrix.

Pull requests and pushes to `main` always run the complete non-FL matrix. The
release workflow remains tag-gated and always builds every distributable
platform, so do not create the version tag until the focused platform checks
and release notes are ready.

## Create a release candidate

Create and push an annotated tag pointing at the reviewed clean commit:

```sh
git tag -a v0.1.0-alpha.2 -m "OpenUtau DAW v0.1.0-alpha.2"
git push origin main
git push origin v0.1.0-alpha.2
```

The tag push starts the workflow automatically. A manual rerun is also
available in Actions, but **Use workflow from** must select that exact tag and
the input must contain the same tag. The workflow rejects branch refs,
lightweight tags, and tags resolving to a different workflow commit.

Signing the Git tag is recommended when a GitHub-recognized signing key is
available, but is not required for this alpha.

## Review and publish

The workflow stops at a draft. Before selecting **Publish release** in GitHub,
confirm:

- all six workflow jobs passed;
- the draft contains macOS arm64, macOS x64, Windows x64, Linux x64, source,
  and five checksum sidecars;
- Windows is labeled experimental in the notes;
- Intel macOS and Linux are labeled experimental in the notes;
- macOS is labeled ad-hoc signed and not Apple-notarized;
- the checksum sidecars match their downloads; and
- the known one-visible-editor limitation remains disclosed.

GitHub-hosted macOS runners are used in host mode because Docker is unavailable
there. Normal development, managed publishing on the target Mac, and the
portable regression suite remain Docker-first.

If the project later obtains Apple Developer credentials, the same macOS
package script supports Developer ID signing, notarization, and stapling with
`CODESIGN_IDENTITY`, `NOTARIZE=1`, and the `NOTARY_*` inputs. Those credentials
must be kept in encrypted Actions secrets and never committed.
