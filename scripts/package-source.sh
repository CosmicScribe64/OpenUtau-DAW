#!/usr/bin/env bash
set -euo pipefail

version="${1:-v0.1.0-alpha.1}"
source_ref="${SOURCE_REF:-HEAD}"
root="$(cd "$(dirname "$0")/.." && pwd)"
juce_source="${JUCE_SOURCE_DIR:-$root/.build/macos-arm64/_deps/juce-src}"
juce_commit="91ad83ae34a81e0833b1a2b0866f54846370ae53"
archive="$root/artifacts/OpenUtau-DAW-$version-source.tar.gz"
checksum="$archive.sha256"
prefix="OpenUtau-DAW-$version"

case "$version" in
  *[!A-Za-z0-9._-]*|'')
    echo "Version contains unsupported archive-name characters: $version" >&2
    exit 1
    ;;
esac

if [[ -n "$(git -C "$root" status --porcelain --untracked-files=all --ignore-submodules=dirty)" ]]; then
  echo "Source packaging requires a clean, committed root worktree." >&2
  exit 1
fi

root_commit="$(git -C "$root" rev-parse "$source_ref^{commit}")"
upstream_commit="$(git -C "$root" ls-tree "$root_commit" upstream | awk '{ print $3 }')"
if [[ -z "$upstream_commit" ]]; then
  echo "The selected source ref does not contain the OpenUtau submodule." >&2
  exit 1
fi
if [[ "$(git -C "$root/upstream" rev-parse HEAD)" != "$upstream_commit" ]]; then
  echo "The populated OpenUtau submodule does not match the selected source ref." >&2
  exit 1
fi
if [[ "$(git -C "$juce_source" rev-parse HEAD)" != "$juce_commit" ]]; then
  echo "JUCE source is absent or not the pinned JUCE 8.0.15 commit." >&2
  exit 1
fi

mkdir -p "$root/artifacts"
temporary="$(mktemp -d "$root/artifacts/.source-package.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
staging="$temporary/staging"
mkdir -p "$staging"

git -C "$root" archive --format=tar --prefix="$prefix/" "$root_commit" | tar -xf - -C "$staging"
git -C "$root/upstream" archive --format=tar --prefix="$prefix/upstream/" "$upstream_commit" | tar -xf - -C "$staging"
git -C "$juce_source" archive --format=tar --prefix="$prefix/third_party/JUCE/" "$juce_commit" | tar -xf - -C "$staging"

{
  echo "OpenUtau DAW $version corresponding source"
  echo "Root commit: $root_commit"
  echo "OpenUtau commit: $upstream_commit"
  echo "JUCE 8.0.15 commit: $juce_commit"
} > "$staging/$prefix/SOURCE-BUILD-INFO.txt"
source_root="$staging/$prefix"
git init -q "$source_root"
printf '* -text\n' > "$source_root/.git/info/attributes"
git -C "$source_root" config core.autocrlf false
git -C "$source_root" add -f .
GIT_AUTHOR_DATE="2000-01-01T00:00:00Z" \
GIT_COMMITTER_DATE="2000-01-01T00:00:00Z" \
  git -C "$source_root" -c user.name="OpenUtau DAW release" \
  -c user.email="release@invalid.example" commit -qm "Corresponding source"
git -C "$source_root" archive --format=tar --prefix="$prefix/" HEAD | gzip -n -9 > "$archive"
(cd "$(dirname "$archive")" && shasum -a 256 "$(basename "$archive")" > "$(basename "$checksum")")

for required in \
  "$prefix/LICENSE" \
  "$prefix/plugin/CMakeLists.txt" \
  "$prefix/patches/openutau-vst.patch" \
  "$prefix/upstream/OpenUtau.sln" \
  "$prefix/third_party/JUCE/LICENSE.md" \
  "$prefix/SOURCE-BUILD-INFO.txt"
do
  if ! tar -tzf "$archive" "$required" >/dev/null; then
    echo "Corresponding-source archive is missing: $required" >&2
    exit 1
  fi
done

echo "Created $archive"
