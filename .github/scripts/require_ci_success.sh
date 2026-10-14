#!/usr/bin/env bash
# Exit 0 only when workflow ci.yml has a successful run on this exact commit.
# Used by version.yml and release.yml so a red main cannot ship.
set -euo pipefail

sha="${1:-}"
if [[ ! "$sha" =~ ^[0-9a-fA-F]{7,40}$ ]]; then
    echo "require_ci_success: need a commit sha, got '${sha}'" >&2
    exit 2
fi

repo="${GH_REPO:-${GITHUB_REPOSITORY:-}}"
if [ -z "$repo" ]; then
    echo "require_ci_success: GH_REPO or GITHUB_REPOSITORY is required" >&2
    exit 2
fi
if [ -z "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ]; then
    echo "require_ci_success: GH_TOKEN or GITHUB_TOKEN is required" >&2
    exit 2
fi
export GH_TOKEN="${GH_TOKEN:-$GITHUB_TOKEN}"

json="$(gh run list \
    --repo "$repo" \
    --commit "$sha" \
    --workflow ci.yml \
    --limit 50 \
    --json conclusion,headSha)"

count="$(python3 -c '
import json, sys
sha = sys.argv[1]
runs = json.load(sys.stdin)
print(sum(1 for r in runs if r.get("conclusion") == "success" and r.get("headSha") == sha))
' "$sha" <<<"$json")"

if [ "${count:-0}" = "0" ]; then
    echo "CI workflow ci.yml has no successful run on ${sha}." >&2
    echo "Version bumps and releases stay blocked until that commit is green." >&2
    echo "Also mark these CI jobs as required status checks on main" >&2
    echo "(Settings → Rules → Rulesets, or classic branch protection):" >&2
    echo "  version-bump-script" >&2
    echo "  macos-arm64" >&2
    echo "  linux-x86_64" >&2
    echo "  linux-x86_64-asan" >&2
    echo "  linux-x86_64-tsan" >&2
    exit 1
fi

echo "CI passed on ${sha} (${count} successful run(s))"
