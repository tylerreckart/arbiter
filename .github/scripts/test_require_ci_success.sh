#!/usr/bin/env bash
# Fixture test for require_ci_success.sh. A fake `gh` on PATH stands in
# for the Actions API so the suite does not need network or a token.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="${ROOT}/.github/scripts/require_ci_success.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/gh" <<'EOF'
#!/usr/bin/env bash
commit=""
prev=""
for a in "$@"; do
    if [ "$prev" = "--commit" ]; then
        commit="$a"
    fi
    prev="$a"
done
case "$commit" in
    aaaaaaa)
        printf '%s\n' '[{"conclusion":"success","headSha":"aaaaaaa"}]'
        ;;
    bbbbbbb)
        printf '%s\n' '[{"conclusion":"failure","headSha":"bbbbbbb"}]'
        ;;
    ccccccc)
        printf '%s\n' '[{"conclusion":"success","headSha":"ddddddd"}]'
        ;;
    *)
        printf '%s\n' '[]'
        ;;
esac
EOF
chmod +x "$TMP/gh" "$SCRIPT"

export PATH="$TMP:$PATH"
export GH_REPO="example/arbiter"
export GH_TOKEN="test-token"

"$SCRIPT" aaaaaaa >/dev/null

set +e
"$SCRIPT" bbbbbbb >/dev/null 2>"$TMP/fail.err"
fail_rc=$?
"$SCRIPT" ccccccc >/dev/null 2>"$TMP/mismatch.err"
mismatch_rc=$?
"$SCRIPT" not-a-sha >/dev/null 2>"$TMP/bad.err"
bad_rc=$?
set -e

if [ "$fail_rc" -eq 0 ] || [ "$mismatch_rc" -eq 0 ] || [ "$bad_rc" -eq 0 ]; then
    echo "require_ci_success accepted a commit that is not green" >&2
    exit 1
fi
if ! grep -q "no successful run" "$TMP/fail.err"; then
    echo "failure case did not explain the block" >&2
    cat "$TMP/fail.err" >&2
    exit 1
fi
echo "require_ci_success: green accepted, red and mismatched SHAs rejected"
