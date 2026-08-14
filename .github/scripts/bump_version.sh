#!/usr/bin/env bash
# Compute and apply the next arbiter semver.
#
# Version lives in CMakeLists.txt (`project(arbiter VERSION X.Y.Z)`) and
# is embedded in the binary as INDEX_VERSION.  Tags are `vX.Y.Z` and
# fire release.yml.  This script is the single place that decides the
# next version so the GitHub workflow and local dry-runs stay in sync.
#
# Auto (mode=auto, the default on merges to main):
#   - If CMakeLists is already ahead of the latest `vX.Y.Z` tag, the
#     merge *was* a manual version bump — use that version, don't bump
#     again.  That is how minor / major land: bump CMakeLists in the
#     PR (or workflow_dispatch), not via the merge itself.
#   - Otherwise bump the patch (0.12.2 → 0.12.3).  Baseline is
#     max(CMakeLists, latest tag) so a stale CMakeLists after a
#     tag-only release still advances.  Auto ignores --bump so a
#     merge cannot accidentally cut a minor.
#
# Manual (mode=manual, workflow_dispatch):
#   - `--bump major|minor|patch` from that same baseline, or
#   - `--version X.Y.Z` for an explicit pin (1.0.0, a skipped patch, …).
#
# Skip (exit 0 with skipped=true) when:
#   - the target tag already exists
#   - mode=auto and --trigger-sha is already contained in a later
#     chore(release) commit (a queued run whose merge was folded into
#     an earlier bump).  Manual dispatch still bumps.
set -euo pipefail

usage() {
  cat <<'EOF' >&2
usage: bump_version.sh [options]

  --mode auto|manual     auto (default): patch bump, honor in-tree version
                         manual: apply --bump / --version
  --bump major|minor|patch   bump type for --mode manual (default: patch)
  --version X.Y.Z        explicit version; overrides --bump
  --trigger-sha SHA      merge SHA that triggered CI; used for skip logic
  --dry-run              print the plan; do not write, commit, or tag
  --no-commit            write files but do not git commit / tag
  --no-tag               commit (if needed) but do not create a tag
EOF
  exit 2
}

MODE="auto"
BUMP="patch"
EXPLICIT=""
TRIGGER_SHA=""
DRY_RUN=0
NO_COMMIT=0
NO_TAG=0

while [ $# -gt 0 ]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --bump)
      BUMP="${2:-}"
      shift 2
      ;;
    --version)
      EXPLICIT="${2:-}"
      shift 2
      ;;
    --trigger-sha)
      TRIGGER_SHA="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-commit)
      NO_COMMIT=1
      shift
      ;;
    --no-tag)
      NO_TAG=1
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "bump_version: unknown argument: $1" >&2
      usage
      ;;
  esac
done

case "$MODE" in
  auto|manual) ;;
  *) echo "bump_version: --mode must be auto or manual" >&2; exit 2 ;;
esac
case "$BUMP" in
  major|minor|patch) ;;
  *) echo "bump_version: --bump must be major, minor, or patch" >&2; exit 2 ;;
esac

# Strip a leading v and reject anything that isn't numeric X.Y.Z.
# Prerelease suffixes belong in ARBITER_VERSION_SUFFIX, not the tag.
normalize_version() {
  local v="${1:-}"
  v="${v#v}"
  if ! [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "bump_version: not a numeric X.Y.Z version: ${1:-<empty>}" >&2
    exit 2
  fi
  printf '%s' "$v"
}

# True if $1 > $2 using version sort (0.9.0 < 0.12.0).
version_gt() {
  local a="$1" b="$2"
  [ "$a" != "$b" ] && [ "$(printf '%s\n%s\n' "$a" "$b" | sort -V | tail -n1)" = "$a" ]
}

read_cmake_version() {
  local v
  v="$(sed -nE 's/^project\(arbiter VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt | head -n1)"
  if [ -z "$v" ]; then
    echo "bump_version: could not read project(arbiter VERSION …) from CMakeLists.txt" >&2
    exit 1
  fi
  printf '%s' "$v"
}

# Highest vX.Y.Z tag.  Ignores prereleases (v0.5.0-beta1) so a leftover
# beta tag cannot become the bump baseline.
latest_tag_version() {
  local tag
  tag="$(git tag --list 'v*.*.*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -n1 || true)"
  if [ -z "$tag" ]; then
    printf ''
    return 0
  fi
  printf '%s' "${tag#v}"
}

bump_semver() {
  local version="$1" kind="$2"
  local major minor patch
  IFS=. read -r major minor patch <<<"$version"
  case "$kind" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
  esac
  printf '%s.%s.%s' "$major" "$minor" "$patch"
}

max_version() {
  local a="$1" b="$2"
  if [ -z "$a" ]; then printf '%s' "$b"; return 0; fi
  if [ -z "$b" ]; then printf '%s' "$a"; return 0; fi
  if version_gt "$a" "$b"; then printf '%s' "$a"; else printf '%s' "$b"; fi
}

is_release_subject() {
  [[ "$1" == chore\(release\):* ]]
}

# A queued auto-run should no-op when a previous bump already landed a
# release commit that contains this merge.  Without this, two merges
# that race would each patch-bump and the second would cut an empty
# vX.Y.(Z+1) on top of the first.
already_released_trigger() {
  local trigger="$1"
  if [ -z "$trigger" ]; then
    return 1
  fi
  if ! git cat-file -e "${trigger}^{commit}" 2>/dev/null; then
    return 1
  fi
  if ! git merge-base --is-ancestor "$trigger" HEAD 2>/dev/null; then
    return 1
  fi
  local subj head_version
  subj="$(git log -1 --format=%s HEAD)"
  if is_release_subject "$subj"; then
    # A push can land the release commit without the tag; retries must
    # not skip until vX.Y.Z exists.
    head_version="$(sed -nE 's/^chore\(release\): v([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' <<<"$subj")"
    if [ -n "$head_version" ] && ! git rev-parse -q --verify "refs/tags/v${head_version}" >/dev/null; then
      return 1
    fi
    return 0
  fi
  if git log --format=%s "${trigger}..HEAD" | grep -q '^chore(release):'; then
    return 0
  fi
  return 1
}

emit_output() {
  local key="$1" value="$2"
  printf '%s=%s\n' "$key" "$value"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    printf '%s=%s\n' "$key" "$value" >>"${GITHUB_OUTPUT}"
  fi
}

skip() {
  local reason="$1"
  echo "bump_version: skip: ${reason}"
  emit_output skipped true
  emit_output version "${2:-}"
  emit_output reason "${reason}"
  exit 0
}

# Move the body of ## [Unreleased] under ## [version] — date, leaving
# an empty Unreleased heading.  No-op if that version heading already
# exists (a prepare-release PR that already cut the notes).
cut_changelog() {
  local version="$1" date="$2"
  python3 - "$version" "$date" <<'PY'
import pathlib, re, sys

version, date = sys.argv[1], sys.argv[2]
path = pathlib.Path("CHANGELOG.md")
text = path.read_text(encoding="utf-8")
heading = f"## [{version}]"
if heading in text:
    sys.exit(0)
pattern = re.compile(
    r"(## \[Unreleased\][^\n]*\n)(.*?)(\n## \[)",
    re.DOTALL,
)
match = pattern.search(text)
if not match:
    sys.exit("CHANGELOG.md: missing ## [Unreleased] section")
body = match.group(2)
if body.strip():
    new_section = f"\n{heading} — {date}\n{body}"
else:
    new_section = f"\n{heading} — {date}\n\n"
replacement = match.group(1) + new_section + match.group(3)
path.write_text(text[: match.start()] + replacement + text[match.end() :], encoding="utf-8")
PY
}

patch_source_pins() {
  local version="$1"
  python3 - "$version" <<'PY'
import pathlib, re, sys

version = sys.argv[1]
tag = f"v{version}"

def sub_one(path, pattern, repl, required=True):
    p = pathlib.Path(path)
    if not p.exists():
        if required:
            sys.exit(f"bump_version: missing {path}")
        return
    text = p.read_text(encoding="utf-8")
    new, n = re.subn(pattern, repl, text, count=1)
    if n != 1:
        sys.exit(f"bump_version: failed to patch {path} ({n} matches)")
    p.write_text(new, encoding="utf-8")

sub_one(
    "CMakeLists.txt",
    r"(project\(arbiter VERSION )[0-9]+\.[0-9]+\.[0-9]+",
    rf"\g<1>{version}",
)
# Final releases drop any leftover prerelease suffix so INDEX_VERSION
# matches the tag.  Manual beta cuts set the suffix in the same PR.
sub_one(
    "CMakeLists.txt",
    r"(set\(ARBITER_VERSION_SUFFIX \")[^\"]*(\"\))",
    r"\g<1>\g<2>",
    required=False,
)
sub_one(
    "web/lib/config.mjs",
    r"(export const binaryRelease = ')v?[0-9]+\.[0-9]+\.[0-9]+(')",
    rf"\g<1>{tag}\g<2>",
    required=False,
)
sub_one(
    "docs/getting-started/local.md",
    r"(ARBITER_VERSION=)v?[0-9]+\.[0-9]+\.[0-9]+",
    rf"\g<1>{tag}",
    required=False,
)
PY
}

CMAKE_VERSION="$(read_cmake_version)"
TAG_VERSION="$(latest_tag_version)"
BASELINE="$(max_version "$CMAKE_VERSION" "$TAG_VERSION")"
if [ -z "$BASELINE" ]; then
  echo "bump_version: no CMakeLists version and no vX.Y.Z tags" >&2
  exit 1
fi

if [ -n "$EXPLICIT" ]; then
  NEXT="$(normalize_version "$EXPLICIT")"
elif [ "$MODE" = "manual" ]; then
  NEXT="$(bump_semver "$BASELINE" "$BUMP")"
elif version_gt "$CMAKE_VERSION" "${TAG_VERSION:-0.0.0}"; then
  # Merge already bumped CMakeLists (prepare-release PR / manual pin).
  NEXT="$CMAKE_VERSION"
else
  # Merges always patch.  Minor/major are --mode manual or an in-tree
  # CMakeLists pin (the branch above).
  NEXT="$(bump_semver "$BASELINE" "patch")"
fi

echo "bump_version: cmake=${CMAKE_VERSION} tag=${TAG_VERSION:-<none>} baseline=${BASELINE} mode=${MODE} next=${NEXT}"

if [ "$MODE" = "auto" ] && already_released_trigger "$TRIGGER_SHA"; then
  skip "trigger ${TRIGGER_SHA} already contained in a later release commit" "$NEXT"
fi

if git rev-parse -q --verify "refs/tags/v${NEXT}" >/dev/null; then
  skip "tag v${NEXT} already exists" "$NEXT"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  echo "bump_version: dry-run; would cut v${NEXT}"
  emit_output skipped false
  emit_output version "$NEXT"
  emit_output dry_run true
  exit 0
fi

TODAY="$(date -u +%F)"
cut_changelog "$NEXT" "$TODAY"
patch_source_pins "$NEXT"

emit_output skipped false
emit_output version "$NEXT"

if [ "$NO_COMMIT" -eq 1 ]; then
  echo "bump_version: wrote files for v${NEXT} (--no-commit)"
  exit 0
fi

if [ -z "$(git status --porcelain CMakeLists.txt CHANGELOG.md web/lib/config.mjs docs/getting-started/local.md 2>/dev/null || true)" ]; then
  echo "bump_version: tree already at ${NEXT}"
else
  git add CMakeLists.txt CHANGELOG.md web/lib/config.mjs docs/getting-started/local.md
  git commit -m "$(cat <<EOF
chore(release): v${NEXT}

Cut CMakeLists INDEX_VERSION, changelog, and install pins to v${NEXT}.
EOF
)"
fi

if [ "$NO_TAG" -eq 1 ]; then
  echo "bump_version: committed v${NEXT} (--no-tag)"
  exit 0
fi

git tag "v${NEXT}"
echo "bump_version: tagged v${NEXT}"
