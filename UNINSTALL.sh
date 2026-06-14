#!/usr/bin/env bash
set -euo pipefail

RED="\033[1;31m"
GREEN="\033[1;32m"
CYAN="\033[1;36m"
YELLOW="\033[33m"
RESET="\033[0m"

TARGET="/opt/KSM"
BIN="/usr/bin"

info() { printf "%b[*]%b %s\n" "$CYAN" "$RESET" "$*"; }
ok() { printf "%b[+]%b %s\n" "$GREEN" "$RESET" "$*"; }
warn() { printf "%b[!]%b %s\n" "$YELLOW" "$RESET" "$*"; }
fail() { printf "%b[x]%b %s\n" "$RED" "$RESET" "$*" >&2; }

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    fail "Run with sudo: sudo bash ./UNINSTALL.sh"
    exit 1
fi

remove_ksm_link() {
    local command="$1"
    local link="$BIN/$command"
    local expected="$TARGET/bin/$command"

    if [ ! -L "$link" ]; then
        return
    fi

    local target
    target="$(readlink "$link" || true)"
    if [ "$target" != "$expected" ]; then
        warn "Skipping $link (not a KSM link)"
        return
    fi

    info "Removing $link"
    rm -f "$link"
}

for command in ksm kcontrol khome kupgr kuninstall ksysinfo kserv kperm kssh kfirewall \
    kgroupadd kgroupmod kgroupdel kuseradd kusermod kuserdel knetcfg; do
    remove_ksm_link "$command"
done

if [ -d "$TARGET" ]; then
    info "Removing $TARGET"
    rm -rf "$TARGET"
    ok "KSM removed."
else
    warn "$TARGET does not exist."
fi

ok "Uninstall complete."
