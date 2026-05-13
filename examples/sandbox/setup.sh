#!/usr/bin/env bash
# examples/sandbox/setup.sh — build, verify, and wire up the example
# sandbox image for `arbiter --api`.
#
# By default: builds the image, smoke-tests it with the same flags the
# runtime will use at /exec time, then prints the env vars to plug
# into arbiter.  Subsequent runs are idempotent — same Dockerfile →
# same image; docker's layer cache makes the second run fast.
#
# Modes (mutually exclusive, default = build+check):
#   --check          Skip the build, just smoke-test an existing image.
#   --teardown       Stop running arbiter-sandbox-* containers and
#                    remove the image.  Workspace bytes are LEFT IN PLACE
#                    under ~/.arbiter/workspaces/ — delete them yourself
#                    if you want a clean slate.
#   --print-only     Skip everything, just print the env-var block.
#
# Flags:
#   --tag NAME       Image tag (default: arbiter/sandbox:latest).
#   --no-cache       docker build --no-cache (force a fresh build).
#   --yes            Skip the teardown confirmation prompt.
#   -h, --help       This help.
#
# Exits 0 on success, non-zero on docker error or smoke-test failure.

set -euo pipefail

TAG="arbiter/sandbox:latest"
NO_CACHE=""
MODE="build"           # build | check | teardown | print
ASSUME_YES=0

usage() {
    sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag)         TAG="$2"; shift 2 ;;
        --tag=*)       TAG="${1#*=}"; shift ;;
        --no-cache)    NO_CACHE="--no-cache"; shift ;;
        --check)       MODE="check"; shift ;;
        --teardown)    MODE="teardown"; shift ;;
        --print-only)  MODE="print"; shift ;;
        --yes|-y)      ASSUME_YES=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Helpers ─────────────────────────────────────────────────────────────

need_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "error: docker not found on PATH." >&2
        echo "       install Docker Desktop or the docker CLI, then re-run." >&2
        exit 1
    fi
    if ! docker info >/dev/null 2>&1; then
        echo "error: docker is installed but the daemon is not reachable." >&2
        echo "       start Docker Desktop (or 'sudo systemctl start docker')," >&2
        echo "       then re-run." >&2
        exit 1
    fi
}

# Run the image with the same flags `arbiter --api` will use at /exec
# time.  Catches "image built but won't run under --read-only", "tmpfs
# mount rejected", "no /bin/sh in the image", etc. — failures that
# would otherwise surface mid-conversation as a confused agent.
smoke_test() {
    echo "==> smoke-testing $TAG with sandbox flags"
    local probe_out
    if ! probe_out=$(docker run --rm \
            --network=none \
            --memory=128m \
            --pids-limit=64 \
            --read-only \
            --tmpfs /tmp:rw,size=8m \
            "$TAG" \
            sh -c 'test -d /tmp && command -v sh >/dev/null && echo ok' \
            2>&1); then
        echo "smoke test FAILED:" >&2
        echo "$probe_out" >&2
        echo
        echo "the image built but won't run with the sandbox's runtime flags." >&2
        echo "common causes: image has no /bin/sh; image expects a writable /;" >&2
        echo "image's base requires capabilities arbiter strips." >&2
        exit 1
    fi
    if [[ "$probe_out" != "ok" ]]; then
        echo "smoke test produced unexpected output: '$probe_out'" >&2
        exit 1
    fi
    echo "    ok"
}

warn_if_arbiter_missing() {
    if ! command -v arbiter >/dev/null 2>&1; then
        echo
        echo "note: 'arbiter' isn't on PATH in this shell.  The build is" >&2
        echo "      done, but you'll need to install or PATH-add the binary" >&2
        echo "      before 'arbiter --api' picks up the env vars below." >&2
    fi
}

print_next_steps() {
    cat <<EOF
==> next steps

  1. Export the sandbox env vars (add to your shell profile or systemd unit):

       export ARBITER_SANDBOX_IMAGE=$TAG

     Optional knobs (defaults shown):
       # export ARBITER_SANDBOX_MEMORY_MB=512                  # hard memory cap
       # export ARBITER_SANDBOX_CPUS=1.0                       # CPU shares
       # export ARBITER_SANDBOX_PIDS_LIMIT=256                 # fork-bomb cap
       # export ARBITER_SANDBOX_EXEC_TIMEOUT=30                # per-/exec wall clock
       # export ARBITER_SANDBOX_NETWORK=none                   # 'bridge' to allow internet
       # export ARBITER_SANDBOX_WORKSPACE_MAX_BYTES=$((1024*1024*1024))  # 1 GiB per tenant
       # export ARBITER_SANDBOX_IDLE_SECONDS=1800              # idle reaper threshold

     Operational env (Phase 1/2):
       # export ARBITER_LOG_FORMAT=json                        # structured stderr for log aggregators
       # export ARBITER_DRAIN_SECONDS=30                       # SIGTERM grace period

  2. Start arbiter:

       arbiter --api

     With the sandbox usable the server logs (human format):

       [HH:MM:SS] [info] sandbox_enabled image=$TAG network=none memory_mb=512 cpus=1.00 pids=256 timeout_s=30

     Or in JSON mode:

       {"ts":"...","level":"info","event":"sandbox_enabled","image":"$TAG",...}

     A failed usability check logs warn event 'sandbox_disabled' with the
     reason and falls back to /exec disabled — the server keeps running.

  3. Confirm the sandbox is wired by issuing /exec from an agent and
     checking for the workspace bind mount inside the container:

       /exec mount | grep /workspace
       /exec id

  4. Scrape metrics from your monitor of choice:

       curl http://127.0.0.1:8080/v1/metrics | grep arbiter_sandbox

  See docs/concepts/sandbox.md for the full walkthrough and
  docs/api/metrics.md for the metrics catalogue.
EOF
}

teardown() {
    need_docker

    local containers
    containers=$(docker ps -a --filter "name=arbiter-sandbox-t" --format '{{.Names}}')

    local actions=()
    [[ -n "$containers" ]] && actions+=("stop + rm $(echo "$containers" | wc -l | tr -d ' ') container(s)")
    if docker image inspect "$TAG" >/dev/null 2>&1; then
        actions+=("remove image $TAG")
    fi
    if [[ ${#actions[@]} -eq 0 ]]; then
        echo "nothing to tear down — no arbiter-sandbox-* containers, no image '$TAG'."
        return 0
    fi

    echo "==> teardown plan"
    for a in "${actions[@]}"; do echo "    - $a"; done
    echo
    echo "    workspace bytes at ~/.arbiter/workspaces/ are LEFT IN PLACE."
    echo

    if [[ "$ASSUME_YES" -ne 1 ]]; then
        read -r -p "proceed? [y/N] " reply
        case "$reply" in
            y|Y|yes|YES) ;;
            *) echo "aborted."; exit 0 ;;
        esac
    fi

    if [[ -n "$containers" ]]; then
        # shellcheck disable=SC2086
        docker rm -f $containers
    fi
    if docker image inspect "$TAG" >/dev/null 2>&1; then
        docker image rm "$TAG"
    fi
    echo "done."
}

# ── Entry points ────────────────────────────────────────────────────────

case "$MODE" in
    print)
        print_next_steps
        ;;
    teardown)
        teardown
        ;;
    check)
        need_docker
        if ! docker image inspect "$TAG" >/dev/null 2>&1; then
            echo "error: image '$TAG' not found.  Build it first:" >&2
            echo "       $(basename "$0")  # or with --tag <name>" >&2
            exit 1
        fi
        smoke_test
        print_next_steps
        ;;
    build)
        need_docker
        echo "==> building $TAG from $script_dir/Dockerfile"
        docker build $NO_CACHE -t "$TAG" "$script_dir"
        echo
        echo "==> verifying image"
        docker image inspect "$TAG" --format \
            'image: {{index .RepoTags 0}}  size: {{.Size}} bytes  created: {{.Created}}'
        echo
        smoke_test
        echo
        warn_if_arbiter_missing
        print_next_steps
        ;;
esac
