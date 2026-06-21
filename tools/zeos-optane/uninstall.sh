#!/usr/bin/env bash
# zeos-optane uninstaller. Removes the package, CLI shim, systemd unit, and
# udev rule. Leaves /etc/zeos/ in place — your Optane configuration is data,
# and you may want to keep it for a future reinstall or for Zeos native.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "error: uninstall.sh must run as root (try: sudo $0)" >&2
    exit 1
fi

INSTALL_LIB="/usr/local/lib/zeos-optane"
INSTALL_BIN="/usr/local/bin/zeos-optane"
SYSTEMD_UNIT="/etc/systemd/system/zeos-optane-detect.service"
UDEV_RULE="/etc/udev/rules.d/99-zeos-optane.rules"

echo ">> uninstalling zeos-optane"

if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    systemctl disable --now zeos-optane-detect.service 2>/dev/null || true
fi
[[ -f "$SYSTEMD_UNIT" ]] && rm -f "$SYSTEMD_UNIT" && echo "   * removed $SYSTEMD_UNIT"

[[ -f "$UDEV_RULE" ]] && rm -f "$UDEV_RULE" && echo "   * removed $UDEV_RULE"
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
fi

[[ -f "$INSTALL_BIN" ]] && rm -f "$INSTALL_BIN" && echo "   * removed $INSTALL_BIN"
[[ -d "$INSTALL_LIB" ]] && rm -rf "$INSTALL_LIB" && echo "   * removed $INSTALL_LIB"

if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    systemctl daemon-reload || true
fi

cat <<'POST'

zeos-optane uninstalled.

The configuration directory /etc/zeos/ was NOT removed. To wipe it too:
    sudo rm -rf /etc/zeos

Mounts created by this tool remain in /etc/fstab and remain mounted.
Backups of /etc/fstab from prior applies are at /etc/fstab.zeos-optane.bak.*
POST
