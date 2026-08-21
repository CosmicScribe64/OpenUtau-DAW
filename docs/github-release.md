# Making a GitHub release

The `Build community release candidate` workflow builds a draft prerelease
from an annotated version tag. It creates packages for macOS arm64, macOS
Intel, Windows x64, and Linux x64, along with checksums and a corresponding-
source archive.

The workflow also runs the container test suite, opens the packaged Windows
editor in a smoke host, runs Steinberg's validator, and checks the Linux
package in a native smoke host. macOS packages are ad-hoc signed. The workflow
does not notarize them.

## Running focused checks

Branch pushes do not run the hosted platform matrix. Use **Build and validate**
when a platform check is needed:

- `container` runs the portable Docker tests;
- `windows-editor` builds Windows and opens the packaged editor;
- `fl-studio-e2e` uses the configured self-hosted FL Studio runner;
- `all` runs the normal cross-platform matrix.

Pull requests and pushes to `main` run the non-FL matrix. The release workflow
always builds every package, so tag only after the focused checks and release
notes are ready.

## Creating the candidate

Create an annotated tag on the commit being released:

```sh
version=v0.1.0-alpha.3
git tag -a "$version" -m "OpenUtau DAW $version"
git push origin main
git push origin "$version"
```

A tag push starts the release workflow. For a manual rerun, choose the same tag
in **Use workflow from** and in the version input. Branches, lightweight tags,
and tags pointing at a different workflow revision are rejected.

## Publishing

The workflow leaves the release as a draft. Before publishing it:

- confirm every job passed;
- download the assets and verify their SHA-256 files;
- check that all four platform packages and the source archive are present;
- make sure the notes distinguish tested and experimental platforms;
- mention that macOS is not notarized and only one embedded editor can be open
  at a time.

Developer ID signing and notarization can be enabled later through the
`CODESIGN_IDENTITY`, `NOTARIZE`, and `NOTARY_*` settings. Keep those
credentials in GitHub Actions secrets.
