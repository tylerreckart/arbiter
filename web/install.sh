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

echo "Downloading $url"
curl -fL "$url" -o "$archive"

tar -xzf "$archive" -C "$tmp"

if [ ! -f "$tmp/arbiter" ]; then
  echo "arbiter installer: release archive did not contain an arbiter binary" >&2
  exit 1
fi

mkdir_cmd="mkdir -p"
install_cmd="install -m 0755"
if [ ! -w "$INSTALL_DIR" ]; then
  if command -v sudo >/dev/null 2>&1; then
    mkdir_cmd="sudo mkdir -p"
    install_cmd="sudo install -m 0755"
  else
    echo "arbiter installer: $INSTALL_DIR is not writable and sudo is unavailable" >&2
    echo "Set INSTALL_DIR to a writable directory and retry." >&2
    exit 1
  fi
fi

$mkdir_cmd "$INSTALL_DIR"
$install_cmd "$tmp/arbiter" "$INSTALL_DIR/arbiter"

echo "Installed arbiter to $INSTALL_DIR/arbiter"
echo "Run: arbiter --init"
