#!/usr/bin/env bash
set -euo pipefail

BLUE="\033[1;34m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
RED="\033[1;31m"
YELLOW="\033[33m"
RESET="\033[0m"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER_SRC="$ROOT_DIR/src/kinstall.cpp"
INSTALLER_BIN="/tmp/ksm-installer.$$"

cleanup() {
    rm -f "$INSTALLER_BIN"
}
trap cleanup EXIT
trap 'printf "%b[x]%b Installer bootstrap failed.\n" "$RED" "$RESET" >&2' ERR

if [ "$(id -u)" -ne 0 ]; then
    printf "%bRun with sudo!%b\n" "$RED" "$RESET"
    printf "Use: sudo ./INSTALL.sh\n"
    exit 1
fi

detect_pm() {
    command -v apt-get >/dev/null 2>&1 && echo "apt" && return
    command -v zypper >/dev/null 2>&1 && echo "zypper" && return
    command -v dnf >/dev/null 2>&1 && echo "dnf" && return
    echo "unknown"
}

install_bootstrap_compiler() {
    local pm answer
    pm="$(detect_pm)"
    if [ "$pm" = "unknown" ]; then
        printf "%bERROR:%b g++ is required to start the interactive installer.\n" "$RED" "$RESET"
        printf "Install g++ first, then run: sudo ./INSTALL.sh\n"
        exit 1
    fi

    printf "%bKSM needs a C++ compiler to start the panel.%b\n" "$CYAN" "$RESET"
    printf "Install bootstrap compiler now? [Y/n] "
    read -r answer
    case "$answer" in
        n|N) exit 1 ;;
    esac

    case "$pm" in
        apt)
            apt-get update -y
            apt-get install -y g++
            ;;
        zypper)
            zypper --non-interactive refresh
            zypper --non-interactive install -y gcc-c++
            ;;
        dnf)
            dnf install -y gcc-c++
            ;;
    esac
    printf "%b[+]%b Bootstrap compiler installed.\n" "$GREEN" "$RESET"
}

if ! command -v g++ >/dev/null 2>&1; then
    install_bootstrap_compiler
fi

if [ ! -f "$INSTALLER_SRC" ]; then
    printf "%bERROR:%b missing src/kinstall.cpp\n" "$RED" "$RESET"
    exit 1
fi

printf "%b[*]%b Building interactive installer panel...\n" "$CYAN" "$RESET"
g++ "$INSTALLER_SRC" -std=c++20 -O2 -I "$ROOT_DIR/src/common" -o "$INSTALLER_BIN"
printf "%b[+]%b Installer panel ready.\n" "$GREEN" "$RESET"
exec "$INSTALLER_BIN" --source "$ROOT_DIR" "$@"
