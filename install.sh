#!/usr/bin/env sh
set -eu

REPO="${ARBITER_REPO:-tylerreckart/arbiter}"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "arbiter installer: missing required command: $1" >&2
    exit 1
  }
}

need curl
need tar
need uname
need mktemp
need sed

os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"

case "$os:$arch" in
  darwin:arm64)
    asset="arbiter-macos-arm64.tar.gz"
    ;;
  linux:x86_64 | linux:amd64)
    asset="arbiter-linux-x86_64.tar.gz"
    ;;
  *)
    echo "arbiter installer: unsupported platform: $os/$arch" >&2
    echo "Build from source instead: https://github.com/$REPO" >&2
    exit 1
    ;;
esac

tmp="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

url="https://github.com/$REPO/releases/latest/download/$asset"
archive="$tmp/$asset"
version="${ARBITER_VERSION:-}"
local_tarball="${ARBITER_TARBALL:-}"

# Require both the archive and its .sha256 sidecar before selecting a release.
# A tag can publish the tarball before checksums land (or vice versa).
release_assets_ready() {
  curl -fsIL "$1" >/dev/null 2>&1 && curl -fsIL "$1.sha256" >/dev/null 2>&1
}

if [ -n "$local_tarball" ]; then
  : # CI and local smoke pass the packaged tarball. No release lookup.
elif [ -n "$version" ]; then
  url="https://github.com/$REPO/releases/download/$version/$asset"
elif ! release_assets_ready "$url"; then
  # A release can be published before its build workflow attaches binaries.
  # In that case, select the newest published release that has this platform
  # asset (and checksum) instead of leaving the stable install URL broken.
  # Don't let a transient API failure abort under `set -e` — fall through to
  # the same "no published binary" error users already see.
  releases="$(curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=10" 2>/dev/null || true)"
  # GitHub may return compact single-line JSON; split before each tag_name
  # so line-oriented sed can extract every release tag.
  tags="$(
    printf '%s' "$releases" |
      sed 's/"tag_name"/\n&/g' |
      sed -n 's/^"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
  )"

  for candidate in $tags; do
    candidate_url="https://github.com/$REPO/releases/download/$candidate/$asset"
    if release_assets_ready "$candidate_url"; then
      version="$candidate"
      url="$candidate_url"
      echo "Latest release has no complete $asset; using newest available binary: $version"
      break
    fi
  done

  if [ -z "$version" ]; then
    echo "arbiter installer: no published binary found for $os/$arch" >&2
    echo "Build from source instead: https://github.com/$REPO" >&2
    exit 1
  fi
fi

verify_checksum() {
  checksum_name="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$tmp" && sha256sum -c "$checksum_name")
  elif command -v shasum >/dev/null 2>&1; then
    (cd "$tmp" && shasum -a 256 -c "$checksum_name")
  else
    echo "arbiter installer: sha256sum or shasum is required to verify the download" >&2
    exit 1
  fi
}

if [ -n "$local_tarball" ]; then
  if [ ! -f "$local_tarball" ]; then
    echo "arbiter installer: ARBITER_TARBALL does not exist: $local_tarball" >&2
    exit 1
  fi
  base="$(basename "$local_tarball")"
  archive="$tmp/$base"
  cp "$local_tarball" "$archive"
  if [ ! -f "${local_tarball}.sha256" ]; then
    echo "arbiter installer: checksum sidecar is required: ${local_tarball}.sha256" >&2
    exit 1
  fi
  cp "${local_tarball}.sha256" "$tmp/$base.sha256"
  verify_checksum "$base.sha256"
  echo "Installing from $local_tarball"
else
  echo "Downloading $url"
  curl -fL "$url" -o "$archive"

  checksum="$tmp/$asset.sha256"
  if curl -fL "$url.sha256" -o "$checksum"; then
    verify_checksum "$asset.sha256"
  else
    echo "arbiter installer: release checksum is unavailable" >&2
    exit 1
  fi
fi

tar -xzf "$archive" -C "$tmp"

if [ ! -f "$tmp/arbiter" ]; then
  echo "arbiter installer: release archive did not contain an arbiter binary" >&2
  exit 1
fi

# INSTALL_DIR is the directory on PATH (default /usr/local/bin). The real
# binary and its shared libraries live in a sibling lib/arbiter directory.
# bin/arbiter is a symlink. $ORIGIN (Linux) and @loader_path (macOS) are
# the real binary's directory, so the libraries resolve next to it.
bin_dir="$INSTALL_DIR"
lib_dir="${ARBITER_LIB_DIR:-$(dirname "$bin_dir")/lib/arbiter}"

parent_dir() {
  d="$1"
  while [ ! -d "$d" ]; do
    d="$(dirname "$d")"
  done
  printf '%s' "$d"
}

need_root=0
if [ ! -w "$(parent_dir "$bin_dir")" ] || [ ! -w "$(parent_dir "$lib_dir")" ]; then
  need_root=1
fi

mkdir_cmd="mkdir -p"
install_cmd="install -m 0755"
link_cmd="ln -sfn"
rm_cmd="rm -f"
if [ "$need_root" -eq 1 ]; then
  if command -v sudo >/dev/null 2>&1; then
    mkdir_cmd="sudo mkdir -p"
    install_cmd="sudo install -m 0755"
    link_cmd="sudo ln -sfn"
    rm_cmd="sudo rm -f"
  else
    echo "arbiter installer: $bin_dir is not writable and sudo is unavailable" >&2
    echo "Set INSTALL_DIR to a writable directory and retry." >&2
    exit 1
  fi
fi

$mkdir_cmd "$bin_dir" "$lib_dir"
$install_cmd "$tmp/arbiter" "$lib_dir/arbiter"

# Shared libraries travel with the binary. Preserve SONAME symlinks
# (libonnxruntime.so.1 → libonnxruntime.so.1.31.0) instead of copying
# the target twice. Older tarballs may omit ONNX Runtime; still install
# what is present so a partial archive does not abort after the binary.
for lib in "$tmp"/libopentui.so "$tmp"/libopentui.dylib "$tmp"/libonnxruntime.so* "$tmp"/libonnxruntime*.dylib; do
  if [ ! -e "$lib" ] && [ ! -L "$lib" ]; then
    continue
  fi
  base="$(basename "$lib")"
  if [ -L "$lib" ]; then
    $link_cmd "$(readlink "$lib")" "$lib_dir/$base"
  else
    $install_cmd "$lib" "$lib_dir/$base"
  fi
done

# Relative link when the payload is the default sibling of INSTALL_DIR.
link_target="$lib_dir/arbiter"
expected="$(dirname "$bin_dir")/lib/arbiter"
if [ "$lib_dir" = "$expected" ]; then
  link_target="../lib/arbiter/arbiter"
fi
$rm_cmd "$bin_dir/arbiter"
$link_cmd "$link_target" "$bin_dir/arbiter"

echo "Installed arbiter to $bin_dir/arbiter"
echo "Libraries: $lib_dir"
echo "Run: arbiter --init"
