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
rm -f "$BIN_DIR"/*
cd "$SRC_DIR"

printf "%b---------------------------Building KSM---------------------------%b\n" "$BLUE" "$RESET"

BUILD_SOURCES="KSM.cpp kcontrol.cpp kupgr.cpp"

build_one() {
    file="$1"
    name="${file%.cpp}"

    case "$name" in
        KSM) out_name="ksm" ;;
        *) out_name="$name" ;;
    esac

    out="/tmp/${out_name}.$$"

    info "Compiling $file -> $out_name"
    if [ "$name" = "kcontrol" ]; then
        if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists ftxui; then
            ftxui_flags="$(pkg-config --cflags --libs ftxui)"
            # shellcheck disable=SC2086
            "$CXX_CMD" "$file" -std=c++20 -O2 -pthread -I "$SRC_DIR/common" -o "$out" $ftxui_flags
        else
            "$CXX_CMD" "$file" -std=c++20 -O2 -pthread -I "$SRC_DIR/common" -o "$out" \
                -lftxui-component -lftxui-dom -lftxui-screen
        fi
    else
        "$CXX_CMD" "$file" -std=c++20 -O2 -I "$SRC_DIR/common" -o "$out"
    fi
    ok "Built $out_name"

    mv -f "$out" "$BIN_DIR/$out_name"
    chmod +x "$BIN_DIR/$out_name"
    ok "Installed $out_name"
}

pids=""
failed=0

for file in $BUILD_SOURCES; do
    if [ ! -f "$file" ]; then
        fail "Missing required source: $file"
        failed=1
        continue
    fi

    build_one "$file" &
    pids="$pids $!"
done

for file in *.cpp; do
    [ -e "$file" ] || continue
    case " $BUILD_SOURCES " in
        *" $file "*) continue ;;
    esac
    warn "Skipping non-public legacy source: $file"
done

for pid in $pids; do
    if ! wait "$pid"; then
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    fail "One or more programs failed to build/install."
    exit 1
fi

printf "%b-----------------------------DONE---------------------------------%b\n" "$GREEN" "$RESET"
ok "Build completed successfully."
