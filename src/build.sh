#!/usr/bin/env bash
set -eu

CXX_CMD="${CXX:-g++}"
TARGET="${KSM_TARGET:-/opt/KSM}"
SRC_DIR="${KSM_SRC_DIR:-$TARGET/src}"
BIN_DIR="$TARGET/bin"

BLUE="\033[1;34m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
RED="\033[1;31m"
YELLOW="\033[33m"
RESET="\033[0m"

info() { printf "%b[*]%b %s\n" "$CYAN" "$RESET" "$*"; }
ok() { printf "%b[+]%b %s\n" "$GREEN" "$RESET" "$*"; }
warn() { printf "%b[!]%b %s\n" "$YELLOW" "$RESET" "$*"; }
fail() { printf "%b[x]%b %s\n" "$RED" "$RESET" "$*" >&2; }

trap 'fail "Build failed."; exit 1' ERR

if ! command -v "$CXX_CMD" >/dev/null 2>&1; then
    fail "Compiler '$CXX_CMD' is not installed."
    exit 1
fi

GCC_VERSION="$("$CXX_CMD" -dumpversion | cut -d. -f1)"
info "Using compiler: $("$CXX_CMD" --version | head -n 1)"

if [ "$GCC_VERSION" -lt 10 ]; then
    fail "KSM requires GCC 10 or newer for C++20."
    exit 1
fi

mkdir -p "$BIN_DIR"
cd "$SRC_DIR"

printf "%b---------------------------Building KSM---------------------------%b\n" "$BLUE" "$RESET"

for file in *.cpp; do
    [ -e "$file" ] || continue

    name="${file%.cpp}"
    if [ "$name" = "kinstall" ]; then
        warn "Skipping installer-only source: $file"
        continue
    fi

    case "$name" in
        KSM) out_name="ksm" ;;
        *) out_name="$name" ;;
    esac

    out="/tmp/${out_name}.$$"
    info "Compiling $file -> $out_name"
    "$CXX_CMD" "$file" -std=c++20 -O2 -I "$SRC_DIR/common" -o "$out"
    mv -f "$out" "$BIN_DIR/$out_name"
    chmod +x "$BIN_DIR/$out_name"
    ok "Installed $out_name"
done

printf "%b-----------------------------DONE---------------------------------%b\n" "$GREEN" "$RESET"
ok "Build completed successfully."
