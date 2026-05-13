# Example arbiter sandbox image.
#
# Built and tagged by `examples/sandbox/setup.sh` as `arbiter/sandbox:latest`.
# Point `ARBITER_SANDBOX_IMAGE` at the tag (or your own derived image) and
# launch `arbiter --api`.  See docs/concepts/sandbox.md for the full
# walkthrough.
#
# Bring-your-own-toolbelt: the contents below are a reasonable starting
# point for general-purpose /exec.  Edit liberally — every agent's
# capability set is different and the sandbox image is where you tune
# what's available behind /exec.
#
# Notes on the runtime contract:
#   * arbiter mounts the per-tenant workspace at /workspace and chdirs
#     into it; nothing else under / is writable.
#   * /tmp is a 64 MB tmpfs (sized by arbiter, not the image).
#   * `--network=none` by default — agents cannot reach the internet
#     from inside /exec.  Pre-install everything you'll need.
#   * The container is kept alive with `sleep infinity`; agents drive
#     work through `docker exec`, not the image's CMD/ENTRYPOINT.

FROM debian:stable-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bash coreutils findutils grep sed gawk less procps \
        ca-certificates curl wget jq \
        git \
        python3 python3-pip \
        build-essential \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# Locale defaults so Python / Click / Rich render UTF-8 cleanly.
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PYTHONUNBUFFERED=1

WORKDIR /workspace
