#!/usr/bin/env bash
set -euo pipefail

BLUE="\033[1;34m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
RED="\033[1;31m"
YELLOW="\033[33m"
RESET="\033[0m"

REPO="Zielina-Konrad-productions/KSM"
API_URL="https://api.github.com/repos/$REPO/releases/latest"
FALLBACK_URL="https://github.com/$REPO/archive/refs/heads/main.tar.gz"

TMP_DIR="$(mktemp -d /tmp/ksm-inetinstall-XXXXXX)"
ARCHIVE="$TMP_DIR/KSM.tar.gz"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT
trap 'printf "%b[x]%b Internet installer failed.\n" "$RED" "$RESET" >&2' ERR

info() { printf "%b[*]%b %s\n" "$CYAN" "$RESET" "$*"; }
ok() { printf "%b[+]%b %s\n" "$GREEN" "$RESET" "$*"; }
warn() { printf "%b[!]%b %s\n" "$YELLOW" "$RESET" "$*"; }
fail() { printf "%b[x]%b %s\n" "$RED" "$RESET" "$*" >&2; }

if [ "$(id -u)" -ne 0 ]; then
    fail "Run with sudo."
    printf "Use:\n"
    printf "  curl -fsSL https://raw.githubusercontent.com/%s/main/INETINSTALL.sh | sudo bash\n" "$REPO"
    exit 1
fi

if ! command -v tar >/dev/null 2>&1; then
    fail "tar is required."
    exit 1
fi

if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    fail "curl or wget is required."
    exit 1
fi

download_stdout() {
    local url="$1"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -H "Accept: application/vnd.github+json" "$url"
        return
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -qO- "$url"
        return
    fi
    return 127
}

download_file() {
    local url="$1"
    local out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$out" "$url"
        return
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -O "$out" "$url"
        return
    fi
    return 127
}

run_interactive_installer() {
    local installer="$1"
    shift

    if [ -r /dev/tty ]; then
        exec bash "$installer" "$@" < /dev/tty
    fi

    fail "Interactive installer needs a real terminal."
    printf "Download KSM and run: sudo bash ./INSTALL.sh\n" >&2
    exit 1
}

release_tarball_url() {
    local json
    if ! json="$(download_stdout "$API_URL" 2>/dev/null)"; then
        return 1
    fi
    printf "%s" "$json" | sed -n 's/.*"tarball_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1
}

printf "%b\n" "$BLUE"
printf "========================================\n"
printf "      Kastiusz System Manager\n"
printf "          Internet Installer\n"
printf "========================================\n"
printf "%b" "$RESET"

URL="$(release_tarball_url || true)"
if [ -z "$URL" ]; then
    warn "No GitHub release tarball found. Falling back to main branch."
    URL="$FALLBACK_URL"
fi

info "Downloading KSM source..."
download_file "$URL" "$ARCHIVE"
ok "Source downloaded."

info "Extracting source..."
tar -xzf "$ARCHIVE" -C "$TMP_DIR"
SOURCE_DIR="$(find "$TMP_DIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)"

if [ -z "$SOURCE_DIR" ] || [ ! -f "$SOURCE_DIR/INSTALL.sh" ]; then
    fail "Downloaded archive does not look like KSM."
    exit 1
fi

chmod +x "$SOURCE_DIR/INSTALL.sh"
ok "Installer ready."
run_interactive_installer "$SOURCE_DIR/INSTALL.sh" "$@"
