#!/usr/bin/env bash
# Shared portable-release helpers used by release.yml and the PR CI
# "portable_release" job.  Keep release packaging logic here so tag
# builds and pull requests exercise the same configure / stage / smoke
# path (Homebrew curl.tbd, static OpenSSL, same-directory OpenTUI).
set -euo pipefail

usage() {
  cat <<'EOF' >&2
usage:
  portable_release.sh configure <build-dir>
  portable_release.sh package <build-dir> <os-slug> <arch-slug>
  portable_release.sh smoke <tarball-name> <opentui-lib-basename>
EOF
  exit 2
}

opentui_lib_for_os() {
  case "$(uname -s)" in
    Darwin) echo "libopentui.dylib" ;;
    Linux)  echo "libopentui.so" ;;
    *)
      echo "portable_release: unsupported OS $(uname -s)" >&2
      exit 1
      ;;
  esac
}

cmd_configure() {
  local build_dir="${1:?build-dir required}"
  local extra=""

  if [ "$(uname -s)" = "Darwin" ]; then
    local openssl_prefix sdkroot
    openssl_prefix="$(brew --prefix openssl@3)"
    sdkroot="$(xcrun --sdk macosx --show-sdk-path)"
    # System libcurl via SDK .tbd — never Homebrew curl (portable install).
    # /usr/lib/libcurl.4.dylib is often only in the dyld shared cache and
    # leaves CURL::libcurl without IMPORTED_LOCATION.
    # Static OpenSSL so users do not need brew's libssl/libcrypto.
    extra="-DOPENSSL_ROOT_DIR=${openssl_prefix}"
    extra="${extra} -DOPENSSL_USE_STATIC_LIBS=TRUE"
    extra="${extra} -DCURL_INCLUDE_DIR=${sdkroot}/usr/include"
    extra="${extra} -DCURL_LIBRARY=${sdkroot}/usr/lib/libcurl.tbd"
    extra="${extra} -DCURL_NO_CURL_CONFIG=ON"
    export PKG_CONFIG_PATH="${openssl_prefix}/lib/pkgconfig"
    unset CURL_ROOT || true
  fi

  # shellcheck disable=SC2086
  cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DINDEX_BUILD_TESTS=OFF \
    ${extra}
}

cmd_package() {
  local build_dir="${1:?build-dir required}"
  local os_slug="${2:?os-slug required}"
  local arch_slug="${3:?arch-slug required}"
  local name="arbiter-${os_slug}-${arch_slug}"
  local stage="stage-${name}"
  local opentui_lib
  opentui_lib="$(opentui_lib_for_os)"

  mkdir -p "${stage}"
  install -m 755 "${build_dir}/arbiter" "${stage}/arbiter"
  if [ ! -f "${build_dir}/${opentui_lib}" ]; then
    echo "portable_release: missing staged OpenTUI library: ${build_dir}/${opentui_lib}" >&2
    ls -la "${build_dir}/" "${build_dir}/opentui/" >&2 || true
    exit 1
  fi
  install -m 755 "${build_dir}/${opentui_lib}" "${stage}/${opentui_lib}"
  [ -f LICENSE ]   && cp LICENSE   "${stage}/"
  [ -f README.md ] && cp README.md "${stage}/"
  tar -C "${stage}" -czf "${name}.tar.gz" .

  if command -v sha256sum >/dev/null; then
    sha256sum "${name}.tar.gz" > "${name}.tar.gz.sha256"
  else
    shasum -a 256 "${name}.tar.gz" > "${name}.tar.gz.sha256"
  fi

  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    {
      echo "name=${name}"
      echo "opentui_lib=${opentui_lib}"
    } >> "${GITHUB_OUTPUT}"
  fi
  echo "portable_release: packaged ${name}.tar.gz (+ ${opentui_lib})"
}

cmd_smoke() {
  local name="${1:?tarball-name required}"
  local opentui_lib="${2:?opentui-lib required}"
  local smoke_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
  local smoke
  smoke="$(mktemp -d "${smoke_root}/arbiter-smoke.XXXXXX")"

  # Extract outside the build tree so a baked-in CI absolute RPATH
  # cannot accidentally satisfy libopentui and greenwash the test.
  tar -xzf "${name}.tar.gz" -C "${smoke}"
  test -x "${smoke}/arbiter"
  test -f "${smoke}/${opentui_lib}"

  if [ "$(uname -s)" = "Darwin" ]; then
    echo "otool -L:"
    otool -L "${smoke}/arbiter" | tee /tmp/arbiter-otool.txt
    # otool -L prints the binary path on the first line (under
    # /Users/runner/work/_temp on GHA). Only inspect indented dependency
    # lines for Homebrew / absolute runner load paths.
    if awk '/^\t/ { print }' /tmp/arbiter-otool.txt \
      | grep -E '/opt/homebrew|/usr/local/opt|/Users/runner'; then
      echo "portable check failed: non-portable dylib load path" >&2
      exit 1
    fi
    grep -E '@(loader_path|rpath)/libopentui\.dylib' /tmp/arbiter-otool.txt
  else
    echo "readelf deps:"
    readelf -d "${smoke}/arbiter" | tee /tmp/arbiter-readelf.txt
    if grep -E 'RPATH|RUNPATH' /tmp/arbiter-readelf.txt | grep -E '/home/runner|/opt/'; then
      echo "portable check failed: non-portable RUNPATH/RPATH" >&2
      exit 1
    fi
    # ldd prints absolute resolved paths (often under /home/runner/work/_temp
    # on GHA). That is expected for $ORIGIN; only reject "not found" or a
    # resolve into the checkout build tree.
    ldd "${smoke}/arbiter" | tee /tmp/arbiter-ldd.txt
    if grep -E 'libopentui\.so => not found' /tmp/arbiter-ldd.txt; then
      echo "portable check failed: libopentui.so not resolved from \$ORIGIN" >&2
      exit 1
    fi
    if ! grep -F "libopentui.so => ${smoke}/${opentui_lib}" /tmp/arbiter-ldd.txt; then
      echo "portable check failed: libopentui.so did not resolve next to arbiter" >&2
      exit 1
    fi
    if grep -E 'libopentui\.so => .*/build/' /tmp/arbiter-ldd.txt; then
      echo "portable check failed: libopentui resolved via build-tree path" >&2
      exit 1
    fi
  fi

  "${smoke}/arbiter" --help >/dev/null
  echo "smoke: --help exit 0 (portable extract)"
}

main() {
  local cmd="${1:-}"
  shift || true
  case "${cmd}" in
    configure) cmd_configure "$@" ;;
    package)   cmd_package "$@" ;;
    smoke)     cmd_smoke "$@" ;;
    *)         usage ;;
  esac
}

main "$@"
