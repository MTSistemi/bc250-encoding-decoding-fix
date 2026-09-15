#!/usr/bin/env bash
# Completely backs out install_vaapi_boot_redirect.sh: stops and disables the
# unit, restores the system's own radeonsi VA-API driver, and removes both
# installed files. Safe to run at any time, including when the unit is
# already masked, already removed, or was never installed.
#
# This exists so that backing the redirect out is a documented one-liner
# rather than something to improvise from a rescue shell. If the machine is
# ALREADY failing to boot and you need the fastest possible escape, you do
# not need this script - from a rescue/emergency shell just run:
#
#     systemctl mask bc250-vaapi-boot-redirect.service
#
# Run as: sudo ./tools/uninstall_vaapi_boot_redirect.sh
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root (sudo)." >&2
    exit 1
fi

UNIT=bc250-vaapi-boot-redirect.service
UNIT_DST=/etc/systemd/system/$UNIT
APPLY_DST=/etc/bc250-vaapi-redirect-apply.sh
TARGET=/usr/lib64/dri/radeonsi_drv_video.so

systemctl stop "$UNIT" 2>/dev/null
systemctl disable "$UNIT" 2>/dev/null
systemctl unmask "$UNIT" 2>/dev/null

rm -f "$UNIT_DST" "$APPLY_DST"
systemctl daemon-reload

# Put the system's own driver back. On ostree a plain reboot would restore
# /usr anyway, but doing it explicitly means the machine is in a clean state
# right now rather than one reboot from now.
if [ -L "$TARGET" ] && readlink "$TARGET" | grep -q bc250_drv_video; then
    if [ ! -w "$(dirname "$TARGET")" ] && command -v rpm-ostree >/dev/null 2>&1; then
        timeout 30 rpm-ostree usroverlay || true
    fi
    STOCK=$(ls /usr/lib64/dri/../libgallium*.so 2>/dev/null | head -1)
    if [ -n "$STOCK" ]; then
        ln -sfn "../$(basename "$STOCK")" "$TARGET" && \
            echo "Restored $TARGET -> ../$(basename "$STOCK")"
    else
        rm -f "$TARGET"
        echo "Removed the bc250 symlink at $TARGET (no stock libgallium found"
        echo "to point back at; a reboot restores /usr on an ostree system)."
    fi
fi

echo
echo "Removed. The bc250 driver itself was NOT deleted - only the boot-time"
echo "redirect. Reinstall it with tools/install_vaapi_boot_redirect.sh."
