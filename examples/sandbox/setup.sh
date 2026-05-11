#!/usr/bin/env bash
# examples/sandbox/setup.sh — build the example sandbox image and
# print the env vars to plug into `arbiter --api`.
#
# Usage:
#   ./setup.sh                       # build with default tag
#   ./setup.sh --tag arbiter/sandbox:dev
#   ./setup.sh --no-cache            # docker build --no-cache
#   ./setup.sh --print-only          # skip build, just print env vars
#
# Exits 0 on success, non-zero on docker errors.  Safe to re-run; the
# build is idempotent (same Dockerfile → same image).

set -euo pipefail

TAG="arbiter/sandbox:latest"
NO_CACHE=""
PRINT_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag) TAG="$2"; shift 2 ;;
        --tag=*) TAG="${1#*=}"; shift ;;
        --no-cache) NO_CACHE="--no-cache"; shift ;;
        --print-only) PRINT_ONLY=1; shift ;;
        -h|--help)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *)
            echo "unknown arg: $1" >&2
            exit 2
            ;;
    esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$PRINT_ONLY" -eq 0 ]]; then
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

    echo "==> building $TAG from $script_dir/Dockerfile"
    docker build $NO_CACHE -t "$TAG" "$script_dir"
    echo
    echo "==> verifying image"
    docker image inspect "$TAG" --format \
        'image: {{.RepoTags}}  size: {{.Size}} bytes  created: {{.Created}}'
    echo
fi

cat <<EOF
==> next steps

  1. Export the sandbox env vars (add to your shell profile or systemd unit):

       export ARBITER_SANDBOX_IMAGE=$TAG
       # optional overrides:
       # export ARBITER_SANDBOX_MEMORY_MB=1024
       # export ARBITER_SANDBOX_CPUS=2
       # export ARBITER_SANDBOX_EXEC_TIMEOUT=60
       # export ARBITER_SANDBOX_NETWORK=bridge          # default: none
       # export ARBITER_SANDBOX_WORKSPACE_MAX_BYTES=$((2*1024*1024*1024))   # 2 GiB
       # export ARBITER_SANDBOX_IDLE_SECONDS=3600       # reap after 1h idle

  2. Start arbiter:

       arbiter --api

     On a clean start with the sandbox usable, the server logs:

       [arbiter] sandbox enabled: image=$TAG network=none memory=512m ...

     A failed usability check (docker missing, image unreachable) logs
     the reason and falls back to /exec disabled — the server keeps
     running.

  3. Confirm it's wired by issuing /exec from an agent and checking
     for the workspace bind mount inside the container:

       /exec mount | grep /workspace
       /exec id

  See docs/concepts/sandbox.md for the full walkthrough.
EOF
