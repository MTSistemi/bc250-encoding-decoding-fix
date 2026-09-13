#!/usr/bin/env bash
# bc250-vaapi-redirect-apply.sh - finds the currently installed
# bc250_drv_video.so, wherever THIS system's installer put it, and symlinks
# the system's default (radeonsi) VA-API driver slot to it.
#
# WHY THIS SEARCHES rather than assumes one path: this project ships three
# different installers, each using a different location -
# setup_bazzite.sh -> /usr/local/lib64/dri, setup_steamos.sh ->
# /var/lib/bc250/dri, build_and_install.sh -> whichever writable candidate it
# finds first - plus /opt/bc250-driver, an older manual convention from this
# project's own development board that no installer here actually creates.
# A real user bug report (CachyOS, driver installed via build_and_install.sh
# at /usr/local/lib64/dri) traced back to this redirect having hardcoded
# /opt/bc250-driver as its ONLY source: that path did not exist on their
# system at all, so the symlink pointed at nothing, and Sunshine's va display
# init failed with a generic "unknown libva error" instead of clearly
# reporting a missing driver. See docs/DEVLOG.md for the full writeup.
#
# Deliberately the SAME candidate list and order tools/bc250_diagnose.sh
# already searches, so "does the diagnostic find the driver" and "does this
# redirect find the driver" can never disagree.
set -uo pipefail

TARGET=/usr/lib64/dri/radeonsi_drv_video.so

CANDIDATES=(
    "/opt/bc250-driver"
    "/var/lib/bc250/dri"
    "/usr/local/lib64/dri"
    "/usr/local/lib/dri"
    "/usr/lib/x86_64-linux-gnu/dri"
    "/usr/lib64/dri"
    "/usr/lib/dri"
)

for dir in "${CANDIDATES[@]}"; do
    if [ -f "$dir/bc250_drv_video.so" ]; then
        ln -sfn "$dir/bc250_drv_video.so" "$TARGET"
        echo "bc250-vaapi-redirect: $TARGET -> $dir/bc250_drv_video.so"
        exit 0
    fi
done

echo "bc250-vaapi-redirect: no installed bc250_drv_video.so found in any" >&2
echo "known location (checked: ${CANDIDATES[*]})." >&2
echo "Run one of the install scripts (build_and_install.sh, setup_bazzite.sh," >&2
echo "setup_steamos.sh) first, then re-run this." >&2
exit 1
