#!/usr/bin/env bash
set -euo pipefail

BLUE="\033[1;34m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
RED="\033[1;31m"
YELLOW="\033[33m"
RESET="\033[0m"

TARGET="/opt/KSM"
BIN_DIR="/usr/bin"
COMMANDS=(ksm khome kupgr kgroupadd kgroupdel kuseradd kuserdel)

info() { printf "%b[*]%b %s\n" "$CYAN" "$RESET" "$*"; }
ok() { printf "%b[+]%b %s\n" "$GREEN" "$RESET" "$*"; }
warn() { printf "%b[!]%b %s\n" "$YELLOW" "$RESET" "$*"; }
fail() { printf "%b[x]%b %s\n" "$RED" "$RESET" "$*" >&2; }

if [ "$(id -u)" -ne 0 ]; then
    fail "Run with sudo."
    printf "Use: sudo bash ./UNINSTALL.sh\n"
    exit 1
fi

printf "%b\n" "$BLUE"
printf "========================================\n"
printf "      Kastiusz System Manager\n"
printf "              Uninstaller\n"
printf "========================================\n"
printf "%b" "$RESET"

printf "This will remove %b%s%b and KSM command links from %b%s%b.\n" "$CYAN" "$TARGET" "$RESET" "$CYAN" "$BIN_DIR" "$RESET"
printf "Continue? [Y/n] "
read -r answer
case "$answer" in
    n|N) warn "Uninstall cancelled."; exit 0 ;;
esac

for command in "${COMMANDS[@]}"; do
    link="$BIN_DIR/$command"
    expected="$TARGET/bin/$command"

    if [ -L "$link" ]; then
        target="$(readlink "$link")"
        if [ "$target" = "$expected" ]; then
            info "Removing link $link"
            rm -f "$link"
            ok "Removed $link"
        else
            warn "Skipping $link; it points to $target"
        fi
    elif [ -e "$link" ]; then
        warn "Skipping non-symlink command: $link"
    fi
done

if [ -d "$TARGET" ]; then
    info "Removing $TARGET"
    rm -rf "$TARGET"
    ok "Removed $TARGET"
else
    warn "$TARGET does not exist."
fi

ok "KSM uninstall complete."
