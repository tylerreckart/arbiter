#!/usr/bin/env bash
# Fixture tests for bump_version.sh.  Builds a throwaway git repo so
# the suite never touches the real working tree.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="${ROOT}/.github/scripts/bump_version.sh"
PASS=0
FAIL=0
CASE_DIR=""

cleanup_case() {
  cd "$ROOT" >/dev/null 2>&1 || true
  if [ -n "${CASE_DIR}" ] && [ -d "${CASE_DIR}" ]; then
    rm -rf "${CASE_DIR}"
  fi
  CASE_DIR=""
}

assert_eq() {
  local label="$1" got="$2" want="$3"
  if [ "$got" = "$want" ]; then
    echo "  ok  ${label}"
    PASS=$((PASS + 1))
  else
    echo "  FAIL ${label}: got '${got}' want '${want}'"
    FAIL=$((FAIL + 1))
  fi
}

assert_file_contains() {
  local label="$1" path="$2" needle="$3"
  if grep -Fq -- "$needle" "$path"; then
    echo "  ok  ${label}"
    PASS=$((PASS + 1))
  else
    echo "  FAIL ${label}: '${path}' missing '${needle}'"
    FAIL=$((FAIL + 1))
  fi
}

assert_file_not_contains() {
  local label="$1" path="$2" needle="$3"
  if grep -Fq -- "$needle" "$path"; then
    echo "  FAIL ${label}: '${path}' unexpectedly has '${needle}'"
    FAIL=$((FAIL + 1))
  else
    echo "  ok  ${label}"
    PASS=$((PASS + 1))
  fi
}

begin_case() {
  echo "case: $1"
  cd "$ROOT"
  cleanup_case
  CASE_DIR="$(mktemp -d)"
  cd "$CASE_DIR"
}

make_fixture() {
  local cmake_ver="$1" tag_ver="${2:-}"
  mkdir -p web/lib docs/getting-started
  cat >CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.20)
project(arbiter VERSION ${cmake_ver} LANGUAGES CXX)
set(ARBITER_VERSION_SUFFIX "")
EOF
  cat >CHANGELOG.md <<'EOF'
# Changelog

## [Unreleased]

### Added
- New thing.

## [0.12.0] — 2026-08-11

Old notes.
EOF
  cat >web/lib/config.mjs <<'EOF'
export const binaryRelease = 'v0.12.0'
EOF
  cat >docs/getting-started/local.md <<'EOF'
curl -fsSL https://arbiter.run/install.sh | ARBITER_VERSION=v0.12.0 sh
EOF
  git init -q
  git config user.email test@example.com
  git config user.name test
  git add .
  git commit -q -m "seed"
  if [ -n "$tag_ver" ]; then
    git tag "v${tag_ver}"
  fi
}

out_field() {
  sed -n "s/^${1}=//p" "$2" | tail -n1
}

trap cleanup_case EXIT

begin_case "auto patch from matching cmake+tag"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.1"
assert_file_contains "cmake" CMakeLists.txt "project(arbiter VERSION 0.12.1"
assert_file_contains "changelog heading" CHANGELOG.md "## [0.12.1] —"
assert_file_contains "changelog kept body" CHANGELOG.md "- New thing."
assert_file_contains "unreleased remains" CHANGELOG.md "## [Unreleased]"
assert_file_contains "site pin" web/lib/config.mjs "v0.12.1"
assert_file_contains "docs pin" docs/getting-started/local.md "ARBITER_VERSION=v0.12.1"

begin_case "auto honors in-tree cmake ahead of tag"
make_fixture 0.12.1 0.12.0
"$SCRIPT" --mode auto --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.1"
assert_file_contains "cmake unchanged" CMakeLists.txt "project(arbiter VERSION 0.12.1"
assert_file_contains "changelog" CHANGELOG.md "## [0.12.1] —"

begin_case "auto baselines off latest tag when cmake is stale"
make_fixture 0.12.0 0.12.2
"$SCRIPT" --mode auto --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.3"

begin_case "auto ignores --bump minor"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto --bump minor --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.1"

begin_case "manual minor bump"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode manual --bump minor --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.13.0"
assert_file_contains "cmake" CMakeLists.txt "project(arbiter VERSION 0.13.0"

begin_case "manual patch bump"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode manual --bump patch --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.1"
assert_file_contains "cmake" CMakeLists.txt "project(arbiter VERSION 0.12.1"

begin_case "manual major bump"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode manual --bump major --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "1.0.0"

begin_case "explicit version overrides bump type"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto --bump minor --version 0.12.5 --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.5"

begin_case "skip explicit version that is already tagged"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode manual --version 0.12.0 >bump.out
assert_eq "skipped" "$(out_field skipped bump.out)" "true"
assert_file_contains "cmake untouched" CMakeLists.txt "project(arbiter VERSION 0.12.0"

begin_case "skip when trigger sha is already in a later release commit"
make_fixture 0.12.0 0.12.0
feature_sha="$(git rev-parse HEAD)"
"$SCRIPT" --mode auto >first.out
assert_eq "first next" "$(out_field version first.out)" "0.12.1"
"$SCRIPT" --mode auto --trigger-sha "$feature_sha" >second.out
assert_eq "second skipped" "$(out_field skipped second.out)" "true"
"$SCRIPT" --mode manual --bump patch --trigger-sha "$feature_sha" --dry-run >manual.out
assert_eq "manual still plans a patch" "$(out_field skipped manual.out)" "false"
assert_eq "manual next" "$(out_field version manual.out)" "0.12.2"

begin_case "does not double-cut changelog if heading exists"
make_fixture 0.13.0 0.12.0
python3 - <<'PY'
from pathlib import Path
p = Path("CHANGELOG.md")
text = p.read_text()
text = text.replace(
    "## [Unreleased]\n\n### Added\n- New thing.\n",
    "## [Unreleased]\n\n## [0.13.0] — 2026-08-12\n\nAlready cut.\n\n",
)
p.write_text(text)
PY
git add CHANGELOG.md
git commit -q -m "cut 0.13.0 notes"
"$SCRIPT" --mode auto --no-commit >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.13.0"
count="$(grep -c '## \[0.13.0\]' CHANGELOG.md || true)"
assert_eq "single heading" "$count" "1"
assert_file_contains "kept existing notes" CHANGELOG.md "Already cut."
assert_file_not_contains "did not resurrect old unreleased" CHANGELOG.md "- New thing."

begin_case "dry-run does not write"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto --dry-run >bump.out
assert_eq "next" "$(out_field version bump.out)" "0.12.1"
assert_file_contains "cmake unchanged" CMakeLists.txt "project(arbiter VERSION 0.12.0"
if git tag --list 'v0.12.1' | grep -q .; then
  echo "  FAIL dry-run created a tag"
  FAIL=$((FAIL + 1))
else
  echo "  ok  no tag"
  PASS=$((PASS + 1))
fi

begin_case "commit + tag on auto"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto >bump.out
assert_eq "tag" "$(git tag --list 'v0.12.1')" "v0.12.1"
assert_eq "subject" "$(git log -1 --format=%s)" "chore(release): v0.12.1"

begin_case "no-tag commits without creating a tag"
make_fixture 0.12.0 0.12.0
"$SCRIPT" --mode auto --no-tag >bump.out
assert_eq "subject" "$(git log -1 --format=%s)" "chore(release): v0.12.1"
if git tag --list 'v0.12.1' | grep -q .; then
  echo "  FAIL --no-tag created a tag"
  FAIL=$((FAIL + 1))
else
  echo "  ok  no tag"
  PASS=$((PASS + 1))
fi

cleanup_case
echo
echo "${PASS} passed, ${FAIL} failed"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
