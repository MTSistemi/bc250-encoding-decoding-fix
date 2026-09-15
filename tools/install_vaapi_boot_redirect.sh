#!/usr/bin/env bash
# Installs bc250-vaapi-boot-redirect.service so the radeonsi VA-API driver
# slot is redirected to bc250_drv_video.so on every boot, not just the
# current one. Needed specifically for Sunshine (and anything else running
# with a `cap_sys_admin`-style file capability): the kernel's secure-exec
# mode makes glibc's secure_getenv() - which libva uses for
# LIBVA_DRIVER_NAME/LIBVA_DRIVERS_PATH precisely because those variables
# control which shared library gets loaded into a privileged process -
# return nothing, regardless of what the shell environment actually holds.
# A plain-old unprivileged consumer (vainfo, ffmpeg, OBS) never hits this;
# only a capability-holding one does. See docs/DEVLOG.md for the full
# root-cause writeup and how this was confirmed.
#
# Run as: sudo ./tools/install_vaapi_boot_redirect.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root (sudo)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_SRC="$SCRIPT_DIR/bc250-vaapi-boot-redirect.service"
UNIT_DST=/etc/systemd/system/bc250-vaapi-boot-redirect.service
APPLY_SRC="$SCRIPT_DIR/bc250-vaapi-redirect-apply.sh"
APPLY_DST=/etc/bc250-vaapi-redirect-apply.sh

if [ ! -f "$UNIT_SRC" ]; then
    echo "Can't find $UNIT_SRC" >&2
    exit 1
fi
if [ ! -f "$APPLY_SRC" ]; then
    echo "Can't find $APPLY_SRC" >&2
    exit 1
fi

# APPLY_DST goes in /etc, not /usr: on this ostree system /usr is reset to a
# fresh read-only overlay every boot (the unit's own usroverlay/ln dance is
# how it deals with that for the symlink itself) but /etc is real, persistent
# state, so the redirect script needs to live there to survive a reboot.
install -m 755 "$APPLY_SRC" "$APPLY_DST"
# 644, not cp: cp preserves the source mode, and a unit file checked out of
# git on a filesystem without exec bits (or from Windows/WSL) lands
# executable, which systemd-analyze verify flags.
install -m 644 "$UNIT_SRC" "$UNIT_DST"
systemctl daemon-reload

# Validate the unit BEFORE enabling it. A malformed or badly-ordered unit
# that is already enabled is a problem you get to discover at the next boot,
# possibly from a rescue shell; caught here it is just an error message.
echo "=== validating the unit before enabling it ==="
if ! systemd-analyze verify "$UNIT_DST"; then
    echo >&2
    echo "systemd-analyze verify reported problems with $UNIT_DST." >&2
    echo "Refusing to enable it. Nothing has been enabled; the unit file is" >&2
    echo "installed but inert. Remove it with:" >&2
    echo "  sudo ./tools/uninstall_vaapi_boot_redirect.sh" >&2
    exit 1
fi
echo "  unit validates clean"

# Start it BEFORE enabling it, so a failure surfaces now, on a running
# system with a shell in front of you, rather than at the next boot.
echo "=== test-starting the unit ==="
if ! systemctl start bc250-vaapi-boot-redirect.service; then
    echo >&2
    echo "The unit failed to start. NOT enabling it at boot." >&2
    systemctl status bc250-vaapi-boot-redirect.service --no-pager >&2 || true
    echo >&2
    echo "Nothing will run at boot. Back it out entirely with:" >&2
    echo "  sudo ./tools/uninstall_vaapi_boot_redirect.sh" >&2
    exit 1
fi

systemctl enable bc250-vaapi-boot-redirect.service

echo "=== verifying ==="
systemctl is-active bc250-vaapi-boot-redirect.service
ls -la /usr/lib64/dri/radeonsi_drv_video.so

echo
echo "Installed and enabled. This reapplies itself on every boot, before the"
echo "display manager starts, so Sunshine (which starts later, in the"
echo "graphical session) always sees the redirect already in place."
echo
echo "If anything ever goes wrong with it:"
echo "  sudo ./tools/uninstall_vaapi_boot_redirect.sh   # full clean removal"
echo "  sudo systemctl mask bc250-vaapi-boot-redirect.service   # fastest stop"
echo
echo "This unit cannot prevent the machine from booting: it runs late (after"
echo "basic.target, wanted by multi-user.target), it is ordered only before"
echo "the display manager, and it has a 45s hard timeout. Worst case is a"
echo "late desktop with SSH available throughout - never an unbootable box."
