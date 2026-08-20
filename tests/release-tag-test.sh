#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
validator="$root/scripts/validate-github-release-tag.sh"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT
repository="$temporary/repository"
gh_stub="$temporary/gh"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'set -euo pipefail' \
  'query="${*: -1}"' \
  'case "$query" in' \
  '  .object.type) printf "%s\\n" "${GH_STUB_TYPE:-tag}" ;;' \
  '  .object.sha) printf "%s\\n" "stub-tag-object" ;;' \
  '  .verification.verified) printf "%s\\n" "${GH_STUB_VERIFIED:-true}" ;;' \
  '  .verification.reason) printf "%s\\n" "${GH_STUB_REASON:-valid}" ;;' \
  '  *) echo "Unexpected gh stub query: $query" >&2; exit 2 ;;' \
  'esac' > "$gh_stub"
chmod +x "$gh_stub"

git init -q "$repository"
git -C "$repository" config user.name "Release test"
git -C "$repository" config user.email "release-test@invalid.example"
printf 'first\n' > "$repository/file.txt"
git -C "$repository" add file.txt
git -C "$repository" commit -qm first
first_commit="$(git -C "$repository" rev-parse HEAD)"
git -C "$repository" tag -a v1.0.0 -m v1.0.0

run_validator() {
  (
    cd "$repository"
    GH_BIN="$gh_stub" "$validator" "$@"
  )
}

expect_failure() {
  description="$1"
  shift
  if "$@" >"$temporary/failure-output" 2>&1; then
    echo "Expected release-tag validation failure: $description" >&2
    exit 1
  fi
}

run_validator v1.0.0 "$first_commit" refs/tags/v1.0.0 owner/repository \
  > "$temporary/success-output"

expect_failure "workflow branch ref" run_validator \
  v1.0.0 "$first_commit" refs/heads/main owner/repository

git -C "$repository" tag lightweight
expect_failure "lightweight tag" run_validator \
  lightweight "$first_commit" refs/tags/lightweight owner/repository

printf 'second\n' >> "$repository/file.txt"
git -C "$repository" add file.txt
git -C "$repository" commit -qm second
second_commit="$(git -C "$repository" rev-parse HEAD)"
expect_failure "tag and workflow commit mismatch" run_validator \
  v1.0.0 "$second_commit" refs/tags/v1.0.0 owner/repository

expect_failure "unverified GitHub signature" env GH_STUB_VERIFIED=false \
  GH_STUB_REASON=unsigned bash -c \
  'cd "$1" && GH_BIN="$2" "$3" v1.0.0 "$4" refs/tags/v1.0.0 owner/repository' \
  release-tag-test "$repository" "$gh_stub" "$validator" "$first_commit"

GH_STUB_VERIFIED=false GH_STUB_REASON=unsigned run_validator \
  v1.0.0 "$first_commit" refs/tags/v1.0.0 owner/repository annotated \
  > "$temporary/annotated-success-output"

expect_failure "unknown verification policy" run_validator \
  v1.0.0 "$first_commit" refs/tags/v1.0.0 owner/repository anything

echo "GitHub release-tag validation tests passed."
