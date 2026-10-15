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
  portable_release.sh install-smoke <tarball-name> <opentui-lib-basename>
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

# The embedding router links ONNX Runtime when configure could fetch it.
# The loader looks up the SONAME next to the binary ($ORIGIN / @loader_path).
# Skip the unversioned libonnxruntime.dylib: that file is a second full
# copy, and the install name is libonnxruntime.1.dylib.
stage_onnxruntime() {
  local build_dir="$1"
  local stage="$2"
  local bin="${build_dir}/arbiter"
  local linked=0
  if [ "$(uname -s)" = "Darwin" ]; then
    if otool -L "${bin}" | grep -q onnxruntime; then
      linked=1
    fi
  elif readelf -d "${bin}" 2>/dev/null | grep -q 'libonnxruntime\.so'; then
    linked=1
  fi
  if [ "${linked}" -eq 0 ]; then
    return 0
  fi

  local libdir=""
  local candidate
  for candidate in "${build_dir}"/embed-fetch/onnxruntime-*/lib; do
    if [ -d "${candidate}" ]; then
      libdir="${candidate}"
      break
    fi
  done
  if [ -z "${libdir}" ]; then
    echo "portable_release: arbiter links ONNX Runtime but embed-fetch has no lib dir" >&2
    exit 1
  fi

  shopt -s nullglob
  local f base staged=0
  for f in "${libdir}"/libonnxruntime.so.* "${libdir}"/libonnxruntime.*.dylib; do
    base="$(basename "${f}")"
    if [ -L "${f}" ]; then
      ln -sfn "$(basename "$(readlink "${f}")")" "${stage}/${base}"
    else
      install -m 755 "${f}" "${stage}/${base}"
    fi
    staged=1
  done
  shopt -u nullglob
  if [ "${staged}" -eq 0 ]; then
    echo "portable_release: arbiter links ONNX Runtime but no library was copied from ${libdir}" >&2
    exit 1
  fi
  echo "portable_release: staged ONNX Runtime from ${libdir}"
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
  normalize_macho_load_paths "${stage}/arbiter"
  stage_onnxruntime "${build_dir}" "${stage}"
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

# ONNX Runtime and OpenTUI must load from @loader_path. An absolute
# install name (the fetch directory) ignores LC_RPATH. Rewrite those
# load commands and leave a single @loader_path rpath.
normalize_macho_load_paths() {
  local bin="$1"
  if [ "$(uname -s)" != "Darwin" ]; then
    return 0
  fi
  local dep base
  while IFS= read -r dep; do
    case "${dep}" in
      @*) continue ;;
      *onnxruntime*|*opentui*)
        base="$(basename "${dep}")"
        install_name_tool -change "${dep}" "@loader_path/${base}" "${bin}"
        ;;
    esac
  done < <(otool -L "${bin}" | awk 'NR>1 { print $1 }')

  local path had_loader=0
  while IFS= read -r path; do
    [ -n "${path}" ] || continue
    if [ "${path}" = "@loader_path" ]; then
      had_loader=1
      continue
    fi
    install_name_tool -delete_rpath "${path}" "${bin}"
  done < <(otool -l "${bin}" | awk '/cmd LC_RPATH/{p=1} p && $1=="path"{print $2; p=0}')
  if [ "${had_loader}" -eq 0 ]; then
    install_name_tool -add_rpath "@loader_path" "${bin}"
  fi
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
    local rpath
    rpath="$(otool -l "${smoke}/arbiter" | awk '/cmd LC_RPATH/{p=1} p && $1=="path"{print $2; p=0}')"
    if [ "${rpath}" != "@loader_path" ]; then
      echo "portable check failed: LC_RPATH must be only @loader_path, got:" >&2
      printf '%s\n' "${rpath}" >&2
      exit 1
    fi
  else
    echo "readelf deps:"
    readelf -d "${smoke}/arbiter" | tee /tmp/arbiter-readelf.txt
    if grep -E 'RPATH|RUNPATH' /tmp/arbiter-readelf.txt | grep -E '/home/runner|/opt/|embed-fetch|/opentui'; then
      echo "portable check failed: non-portable RUNPATH/RPATH" >&2
      exit 1
    fi
    local rp
    rp="$(sed -n 's/.*\(RPATH\|RUNPATH\).*\[\(.*\)\]/\2/p' /tmp/arbiter-readelf.txt)"
    if [ "${rp}" != '$ORIGIN' ]; then
      echo "portable check failed: runtime path must be exactly \$ORIGIN, got: ${rp}" >&2
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
    if grep -q 'libonnxruntime\.so' /tmp/arbiter-readelf.txt; then
      if grep -E 'libonnxruntime\.so[^ ]* => not found' /tmp/arbiter-ldd.txt; then
        echo "portable check failed: libonnxruntime not resolved from \$ORIGIN" >&2
        exit 1
      fi
      if ! grep -E "libonnxruntime\.so\.1 => ${smoke}/libonnxruntime\.so" /tmp/arbiter-ldd.txt; then
        echo "portable check failed: libonnxruntime did not resolve next to arbiter" >&2
        exit 1
      fi
    fi
  fi
  if [ "$(uname -s)" = "Darwin" ] && grep -q onnxruntime /tmp/arbiter-otool.txt; then
    if [ ! -e "${smoke}/libonnxruntime.1.dylib" ]; then
      echo "portable check failed: libonnxruntime.1.dylib missing beside arbiter" >&2
      exit 1
    fi
  fi

  "${smoke}/arbiter" --help >/dev/null
  echo "smoke: --help exit 0 (portable extract)"
}

# Install the packaged tarball with install.sh into a temp prefix and run
# the installed command. Proves shared libraries are copied, not only that
# the extracted archive starts.
cmd_install_smoke() {
  local name="${1:?tarball-name required}"
  local opentui_lib="${2:?opentui-lib required}"
  local root
  root="$(mktemp -d "${TMPDIR:-/tmp}/arbiter-install.XXXXXX")"
  if [ ! -f "${name}.tar.gz" ] || [ ! -f "${name}.tar.gz.sha256" ]; then
    echo "install-smoke: missing ${name}.tar.gz or its .sha256" >&2
    exit 1
  fi
  ARBITER_TARBALL="${PWD}/${name}.tar.gz" INSTALL_DIR="${root}/bin" \
    sh ./install.sh
  test -L "${root}/bin/arbiter"
  test -x "${root}/lib/arbiter/arbiter"
  test -f "${root}/lib/arbiter/${opentui_lib}"
  if tar -tzf "${name}.tar.gz" | grep -q 'libonnxruntime'; then
    if [ "$(uname -s)" = "Darwin" ]; then
      test -e "${root}/lib/arbiter/libonnxruntime.1.dylib"
    else
      test -e "${root}/lib/arbiter/libonnxruntime.so.1"
    fi
  fi
  "${root}/bin/arbiter" --help >/dev/null
  echo "install-smoke: installed binary --help exit 0 (${root}/bin/arbiter)"
}

main() {
  local cmd="${1:-}"
  shift || true
  case "${cmd}" in
    configure)     cmd_configure "$@" ;;
    package)       cmd_package "$@" ;;
    smoke)         cmd_smoke "$@" ;;
    install-smoke) cmd_install_smoke "$@" ;;
    *)             usage ;;
  esac
}

main "$@"
