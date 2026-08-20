# GitHub release setup

The `Build signed release candidate` workflow creates a draft prerelease. It
runs the Docker regression suite, builds macOS arm64 on GitHub's arm64 macOS 15
runner, signs with hardened runtime, submits to Apple's notary service, staples
and verifies the ticket, creates a complete corresponding-source archive that
includes OpenUtau and JUCE source, and uploads checksummed assets.

Configure these repository Actions secrets before running it:

- `MACOS_CERTIFICATE_P12_BASE64` — base64-encoded Developer ID Application
  certificate and private key exported as PKCS#12;
- `MACOS_CERTIFICATE_PASSWORD` — password for that PKCS#12 file;
- `MACOS_SIGNING_IDENTITY` — the complete `Developer ID Application: …` name;
- `NOTARY_API_KEY_P8_BASE64` — base64-encoded App Store Connect API private key;
- `NOTARY_API_KEY_ID` — API key identifier;
- `NOTARY_API_ISSUER_ID` — App Store Connect issuer UUID.

Run the workflow manually with `v0.1.0-alpha.1`. It creates or updates a
**draft** release only. Review the attached binary, checksum, source archive,
licences, known limitations, and retained workflow evidence before manually
publishing the draft.

GitHub-hosted macOS runners are intentionally used in host mode because Docker
is unavailable there. Normal development, managed publishing on the target
Mac, and the canonical regression suite remain Docker-first.
