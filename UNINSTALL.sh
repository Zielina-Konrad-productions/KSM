#!/usr/bin/env bash
set -euo pipefail

RED="\033[1;31m"
RESET="\033[0m"

INSTALLED_UNINSTALLER="/opt/KSM/bin/kuninstall"

fail() { printf "%b[x]%b %s\n" "$RED" "$RESET" "$*" >&2; }

if [ -x "$INSTALLED_UNINSTALLER" ]; then
    exec "$INSTALLED_UNINSTALLER" "$@"
fi

if command -v kuninstall >/dev/null 2>&1; then
    exec kuninstall "$@"
fi

fail "kuninstall is not installed yet."
printf "Install KSM first, or remove files manually from /opt/KSM.\n"
exit 1
