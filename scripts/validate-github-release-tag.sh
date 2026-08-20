#!/usr/bin/env bash
set -euo pipefail

release_tag="${1:?Expected the release tag.}"
expected_commit="${2:?Expected the workflow commit.}"
workflow_ref="${3:?Expected the workflow Git ref.}"
repository="${4:?Expected the GitHub owner/repository.}"
gh_bin="${GH_BIN:-gh}"

case "$release_tag" in
  *[!A-Za-z0-9._-]*|'')
    echo "Invalid release tag: $release_tag" >&2
    exit 1
    ;;
esac

if [[ "$workflow_ref" != "refs/tags/$release_tag" ]]; then
  echo "Run this workflow from the existing $release_tag tag, not $workflow_ref." >&2
  exit 1
fi
if [[ "$(git cat-file -t "refs/tags/$release_tag")" != tag ]]; then
  echo "$release_tag must be an annotated tag." >&2
  exit 1
fi

tag_commit="$(git rev-parse "refs/tags/$release_tag^{commit}")"
if [[ "$tag_commit" != "$expected_commit" ]]; then
  echo "$release_tag resolves to $tag_commit, not workflow commit $expected_commit." >&2
  exit 1
fi

tag_reference="repos/$repository/git/ref/tags/$release_tag"
tag_object_type="$("$gh_bin" api "$tag_reference" --jq '.object.type')"
tag_object_sha="$("$gh_bin" api "$tag_reference" --jq '.object.sha')"
if [[ "$tag_object_type" != tag ]]; then
  echo "$release_tag is not an annotated GitHub tag object." >&2
  exit 1
fi

tag_object="repos/$repository/git/tags/$tag_object_sha"
tag_verified="$("$gh_bin" api "$tag_object" --jq '.verification.verified')"
tag_reason="$("$gh_bin" api "$tag_object" --jq '.verification.reason')"
if [[ "$tag_verified" != true ]]; then
  echo "$release_tag does not have a GitHub-verified signature: $tag_reason" >&2
  exit 1
fi

echo "Verified signed release tag $release_tag at $expected_commit."
