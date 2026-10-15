#!/usr/bin/env bash
# install.sh copies the binary and shared libraries into lib/arbiter and
# symlinks bin/arbiter. No network and no real arbiter binary.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

src="${work}/src"
mkdir -p "${src}"
cat >"${src}/arbiter" <<'EOF'
#!/bin/sh
echo arbiter-ok
EOF
chmod 755 "${src}/arbiter"
printf 'opentui\n' >"${src}/libopentui.so"
printf 'ort-real\n' >"${src}/libonnxruntime.so.1.31.0"
ln -s libonnxruntime.so.1.31.0 "${src}/libonnxruntime.so.1"
printf 'ort-dylib\n' >"${src}/libonnxruntime.1.dylib"

name="arbiter-linux-x86_64"
tar -C "${src}" -czf "${work}/${name}.tar.gz" .
(
  cd "${work}"
  if command -v sha256sum >/dev/null; then
    sha256sum "${name}.tar.gz" >"${name}.tar.gz.sha256"
  else
    shasum -a 256 "${name}.tar.gz" >"${name}.tar.gz.sha256"
  fi
)

prefix="${work}/prefix"
ARBITER_TARBALL="${work}/${name}.tar.gz" INSTALL_DIR="${prefix}/bin" \
  sh "${root}/install.sh"

test -L "${prefix}/bin/arbiter"
test -x "${prefix}/lib/arbiter/arbiter"
test -f "${prefix}/lib/arbiter/libopentui.so"
test -L "${prefix}/lib/arbiter/libonnxruntime.so.1"
test -f "${prefix}/lib/arbiter/libonnxruntime.so.1.31.0"
test -f "${prefix}/lib/arbiter/libonnxruntime.1.dylib"
test "$(readlink "${prefix}/lib/arbiter/libonnxruntime.so.1")" = "libonnxruntime.so.1.31.0"
test "$(readlink "${prefix}/bin/arbiter")" = "../lib/arbiter/arbiter"
got="$("${prefix}/bin/arbiter")"
test "${got}" = "arbiter-ok"
echo "install layout ok"
