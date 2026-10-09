#!/bin/sh
# Runs against TARGET_DIR before the ext4 image is assembled.
set -eu

TARGET_DIR="$1"

# Mountpoint for the FAT config partition, and where sdcard-config-apply
# writes dropbear's args.
mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/etc/default"

# The root runs from RAM: SSH host keys live on the persistent /var
# (dropbear.service.d/30-chumby.conf creates the directory).
rm -rf "${TARGET_DIR}/etc/dropbear"
ln -sfn /var/lib/dropbear "${TARGET_DIR}/etc/dropbear"
mkdir -p "${TARGET_DIR}/var/lib/chumby"

# systemd units replace the shared overlay's sysvinit scripts.
rm -f "${TARGET_DIR}"/etc/init.d/S[0-9]*

GIT_DIR="${BR2_EXTERNAL_PIFANSRF_PATH:-.}"
COMMIT=unknown
DIRTY=
if git -C "${GIT_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
    COMMIT=$(git -C "${GIT_DIR}" rev-parse --short HEAD)
    if ! git -C "${GIT_DIR}" diff --quiet 2>/dev/null \
       || ! git -C "${GIT_DIR}" diff --cached --quiet 2>/dev/null; then
        DIRTY="-dirty"
    fi
fi
cat > "${TARGET_DIR}/etc/motd" <<MOTD

  falconwing (Chumby One) — buildroot, mainline kernel
  build:  ${COMMIT}${DIRTY}
  date:   $(date -u +"%Y-%m-%dT%H:%M:%SZ")

MOTD
