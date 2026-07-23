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

# Require both the archive and its .sha256 sidecar before selecting a release.
# A tag can publish the tarball before checksums land (or vice versa).
release_assets_ready() {
  curl -fsIL "$1" >/dev/null 2>&1 && curl -fsIL "$1.sha256" >/dev/null 2>&1
}

if [ -n "$version" ]; then
  url="https://github.com/$REPO/releases/download/$version/$asset"
elif ! release_assets_ready "$url"; then
  # A release can be published before its build workflow attaches binaries.
  # In that case, select the newest published release that has this platform
  # asset (and checksum) instead of leaving the stable install URL broken.
  releases="$(curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=10")"
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

echo "Downloading $url"
curl -fL "$url" -o "$archive"

checksum="$tmp/$asset.sha256"
if curl -fL "$url.sha256" -o "$checksum"; then
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$tmp" && sha256sum -c "$asset.sha256")
  elif command -v shasum >/dev/null 2>&1; then
    (cd "$tmp" && shasum -a 256 -c "$asset.sha256")
  else
    echo "arbiter installer: sha256sum or shasum is required to verify the download" >&2
    exit 1
  fi
else
  echo "arbiter installer: release checksum is unavailable" >&2
  exit 1
fi

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
