#!/usr/bin/env bash
# Offline check that a SHA-256 mismatch fails the same helper OpenTUI and
# Zig downloads use, and that a matching digest is accepted.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

printf 'hello' > "$TMP/blob"
GOOD=2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
BAD=0000000000000000000000000000000000000000000000000000000000000000

cmake -DFILE="$TMP/blob" -DEXPECTED="$GOOD" -DLABEL=match \
    -P "$ROOT/cmake/test_verify_sha256.cmake"

set +e
out="$(cmake -DFILE="$TMP/blob" -DEXPECTED="$BAD" -DLABEL=mismatch \
    -P "$ROOT/cmake/test_verify_sha256.cmake" 2>&1)"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo "SHA256 mismatch was accepted" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q "SHA256 mismatch"; then
    echo "mismatch did not report SHA256 mismatch:" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if [ -f "$TMP/blob" ]; then
    echo "mismatch left the bad file in place" >&2
    exit 1
fi
echo "sha256 pin: match accepted, mismatch rejected"
