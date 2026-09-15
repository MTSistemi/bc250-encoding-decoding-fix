#!/usr/bin/env bash
# bc250_uninstall.sh - remove everything any of this project's installers put
# on the system, and explain each thing as it goes.
#
# Why this exists: the driver is installed by one of three installers
# (build_and_install.sh, setup_bazzite.sh, setup_steamos.sh) plus an optional
# boot-redirect unit, and between them they write to up to eight DRI
# directories, three shader directories, three environment files and two
# systemd-adjacent paths. Nobody should have to reconstruct that list by hand
# to back the driver out.
#
# This searches for what is actually present rather than reading a manifest,
# so it also cleans up installs made before this script existed.
#
# Usage:
#   sudo ./tools/bc250_uninstall.sh --dry-run    # show what would go, touch nothing
#   sudo ./tools/bc250_uninstall.sh              # do it
#
# It is deliberately conservative: it only ever removes files named
# bc250_drv_video.so / *.spv in bc250-specific directories, its own env files,
# and its own systemd unit. It never deletes a system directory, and it only
# touches the radeonsi driver slot if that slot currently points at us.
set -uo pipefail

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

c_ok=$'\e[32m'; c_warn=$'\e[33m'; c_hdr=$'\e[1;36m'; c_dim=$'\e[2m'; c_off=$'\e[0m'
hdr() { echo; echo "${c_hdr}== $* ==${c_off}"; }
act() { echo "  ${c_ok}removed${c_off} $*"; }
skip() { echo "  ${c_dim}absent ${c_off} $*"; }
warn() { echo "  ${c_warn}! $*${c_off}"; }
note() { echo "    ${c_dim}$*${c_off}"; }

if [ "$(id -u)" -ne 0 ] && [ "$DRY" -eq 0 ]; then
    echo "Run as root (sudo), or pass --dry-run to preview." >&2
    exit 1
fi

# Takes the PATH only. An earlier version was `run rm -f "$path"` and printed
# "$2", which is the -f flag, not the path - so a dry run announced
# "would remove -f" for every file.
rm_path() {
    if [ "$DRY" -eq 1 ]; then
        echo "  ${c_dim}[dry-run] would remove${c_off} $1"
    elif rm -f "$1" 2>/dev/null; then
        act "$1"
    else
        warn "could not remove $1"
    fi
}

FOUND_ANY=0

# ---------------------------------------------------------------- driver .so
hdr "1. Driver binaries"
note "The .so each installer copied into libva's driver search directories."
note "libva finds a driver by filename, so these are what LIBVA_DRIVER_NAME=bc250 loads."
for d in /usr/lib/x86_64-linux-gnu/dri /usr/lib64/dri /usr/lib/dri \
         /usr/local/lib64/dri /usr/local/lib/dri /var/lib/bc250/dri \
         /usr/lib32/dri /usr/lib/i386-linux-gnu/dri; do
    so="$d/bc250_drv_video.so"
    if [ -e "$so" ] || [ -L "$so" ]; then
        FOUND_ANY=1
        rm_path "$so"
    else
        skip "$so"
    fi
done

# /opt/bc250-driver is a manual layout, not something any installer here
# creates - but tools/bc250-vaapi-redirect-apply.sh does search it, so an
# install can legitimately live there. Report it instead of deleting it: this
# script's contract is that it only removes what the installers put down.
if [ -f /opt/bc250-driver/bc250_drv_video.so ]; then
    FOUND_ANY=1
    warn "found /opt/bc250-driver/bc250_drv_video.so - NOT removed"
    note "That path is a manual convention, not installer-created, so it is left"
    note "alone. If you want it gone: sudo rm -rf /opt/bc250-driver"
fi

# ------------------------------------------------------------------- shaders
hdr "2. Compiled shaders"
note "Architecture-independent SPIR-V. Shared by the 32- and 64-bit drivers,"
note "which is why there is one copy per prefix rather than one per driver."
for d in /usr/share/bc250/shaders /usr/local/share/bc250/shaders /var/lib/bc250/shaders; do
    # -L BEFORE -d, and this ordering is load-bearing. [ -d ] is TRUE for a
    # symlink pointing at a directory, so testing -d first would send a
    # symlinked shader dir down the "delete the .spv files inside" path, and
    # the glob would resolve THROUGH the link and delete the real driver's
    # shaders at the far end. /var/lib/bc250/shaders is exactly that on a
    # machine where it was linked at the install directory. Caught by
    # dry-running this against a real install, where it reported 14 files it
    # had no business touching.
    if [ -L "$d" ]; then
        FOUND_ANY=1
        note "$d is a symlink -> $(readlink "$d"); removing the link only, not its target"
        rm_path "$d"
    elif [ -d "$d" ] && [ -n "$(ls -A "$d" 2>/dev/null)" ]; then
        FOUND_ANY=1
        if [ "$DRY" -eq 1 ]; then
            echo "  ${c_dim}[dry-run] would remove${c_off} $d/ ($(ls -1 "$d" | wc -l) files)"
        else
            rm -f "$d"/*.spv "$d"/*.comp 2>/dev/null
            if rmdir "$d" 2>/dev/null; then
                act "$d/"
            else
                act "$d/*.spv (directory kept - it holds files we did not install)"
            fi
        fi
    else
        skip "$d/"
    fi
done

# --------------------------------------------------------------- environment
hdr "3. Environment configuration"
note "This is the part that affects the WHOLE system, not just the encoder:"
note "LIBVA_DRIVER_NAME=bc250 points every VA-API client at this driver, and"
note "this driver advertises encode only. Removing it gives other apps their"
note "hardware video DECODE back."
for f in /etc/environment.d/99-bc250.conf /etc/profile.d/bc250.sh; do
    if [ -f "$f" ]; then FOUND_ANY=1; rm_path "$f"; else skip "$f"; fi
done

# /etc/environment is a SYSTEM file that build_and_install.sh appends to as a
# fallback - delete only our own lines out of it, never the file.
if [ -f /etc/environment ] && grep -qE '^(LIBVA_DRIVER_NAME=bc250|BC250_)' /etc/environment 2>/dev/null; then
    FOUND_ANY=1
    # The LIBVA_DRIVERS_PATH line we write contains no literal "bc250" - it is
    # just a list of DRI directories - so it cannot be matched on that. Match
    # the distinctive prefix build_and_install.sh actually writes instead
    # (both the pre- and post-32-bit forms start with this pair). Narrow on
    # purpose: a LIBVA_DRIVERS_PATH the user set for some other driver must
    # survive, and this whole block only runs when our own marker lines are
    # present anyway.
    OURS='^(LIBVA_DRIVER_NAME=bc250|BC250_[A-Z_]*=|LIBVA_DRIVERS_PATH=/usr/local/lib64/dri:/usr/local/lib/dri:)'
    if [ "$DRY" -eq 1 ]; then
        echo "  ${c_dim}[dry-run] would strip these lines from /etc/environment:${c_off}"
        grep -nE "$OURS" /etc/environment | sed 's/^/      /'
    else
        cp -a /etc/environment /etc/environment.bc250-backup
        # \%...% delimiter, not /.../ : the pattern contains directory paths,
        # and an unescaped / inside a sed address terminates it early
        # ("unmatched ("). And the exit status is checked rather than assumed -
        # a failing sed here would otherwise leave the file untouched while
        # this script cheerfully reported the lines removed.
        if sed -i -E "\%${OURS}%d" /etc/environment; then
            act "bc250 lines from /etc/environment (backup: /etc/environment.bc250-backup)"
        else
            warn "failed to edit /etc/environment - it is unchanged."
            warn "Remove these lines by hand:"
            grep -nE "$OURS" /etc/environment | sed 's/^/      /'
        fi
    fi
else
    skip "bc250 lines in /etc/environment"
fi

# A LIBVA_DRIVERS_PATH we wrote may list only standard dirs and not mention
# bc250 - flag it rather than pattern-guess at someone else's line.
if [ -f /etc/environment ] && grep -q '^LIBVA_DRIVERS_PATH=' /etc/environment 2>/dev/null; then
    warn "/etc/environment still has a LIBVA_DRIVERS_PATH line - check it by hand:"
    grep -n '^LIBVA_DRIVERS_PATH=' /etc/environment | sed 's/^/      /'
fi

# ------------------------------------------------------- boot redirect unit
hdr "4. Boot-time driver redirect (Sunshine/Steam Link workaround)"
note "A systemd unit that re-points the system's radeonsi VA-API slot at this"
note "driver on every boot, for clients whose environment libva ignores."
UNIT=bc250-vaapi-boot-redirect.service
if systemctl list-unit-files 2>/dev/null | grep -q "^$UNIT"; then
    FOUND_ANY=1
    if [ "$DRY" -eq 1 ]; then
        echo "  ${c_dim}[dry-run] would stop/disable/unmask${c_off} $UNIT"
    else
        systemctl stop "$UNIT" 2>/dev/null
        systemctl disable "$UNIT" 2>/dev/null
        systemctl unmask "$UNIT" 2>/dev/null
        act "$UNIT (stopped, disabled, unmasked)"
    fi
else
    skip "$UNIT"
fi
for f in /etc/systemd/system/$UNIT /etc/systemd/system/$UNIT.disabled \
         /etc/bc250-vaapi-redirect-apply.sh /etc/bc250-uninstall-vaapi-boot-redirect.sh; do
    if [ -e "$f" ]; then FOUND_ANY=1; rm_path "$f"; else skip "$f"; fi
done
[ "$DRY" -eq 0 ] && systemctl daemon-reload 2>/dev/null

# ------------------------------------------- restore the stock radeonsi slot
hdr "5. System radeonsi VA-API driver"
note "Only touched if it currently points at us. On an ostree system a reboot"
note "would restore /usr anyway; this makes the machine correct right now."
TARGET=/usr/lib64/dri/radeonsi_drv_video.so
if [ -L "$TARGET" ] && readlink "$TARGET" | grep -q bc250_drv_video; then
    FOUND_ANY=1
    if [ "$DRY" -eq 1 ]; then
        echo "  ${c_dim}[dry-run] would restore${c_off} $TARGET (currently -> $(readlink "$TARGET"))"
    else
        if [ ! -w "$(dirname "$TARGET")" ] && command -v rpm-ostree >/dev/null 2>&1; then
            timeout 30 rpm-ostree usroverlay >/dev/null 2>&1 || true
        fi
        STOCK=$(ls /usr/lib64/libgallium*.so 2>/dev/null | head -1)
        if [ -n "$STOCK" ]; then
            ln -sfn "../$(basename "$STOCK")" "$TARGET" && act "$TARGET -> ../$(basename "$STOCK") (stock Mesa)"
        else
            rm -f "$TARGET" && act "$TARGET (no stock libgallium found to restore; a reboot restores /usr on ostree)"
        fi
    fi
else
    skip "$TARGET (not pointing at bc250)"
fi

# --------------------------------------------------------------- not ours
hdr "6. Not removed by this script"
note "audio-fix is a DKMS kernel module with its own uninstaller:"
echo "      sudo ./audio-fix/uninstall_dkms.sh"
note "Anything you set by hand (a Sunshine/WiVRn unit override, a systemd"
note "drop-in, kernel args) is left alone - this script only removes what the"
note "project's own installers create."

# --------------------------------------------------------------- summary
hdr "Summary"
if [ "$FOUND_ANY" -eq 0 ]; then
    echo "  Nothing from this project was found on the system."
elif [ "$DRY" -eq 1 ]; then
    echo "  Dry run - nothing was changed. Re-run without --dry-run to apply."
else
    echo "  Done. Verify with:"
    echo "      LIBVA_DRIVER_NAME=radeonsi vainfo      # stock driver should list decode entrypoints again"
    echo "      ls /usr/lib*/dri/bc250_drv_video.so    # should be gone"
    echo
    echo "  Environment changes only affect NEWLY started processes, so log out"
    echo "  and back in (or reboot) before judging whether they took effect."
fi
echo
