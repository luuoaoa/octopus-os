#!/bin/sh
# Runs against TARGET_DIR before the ext4 image is assembled.
set -eu

TARGET_DIR="$1"

# Mountpoint for the FAT config partition, and where sdcard-config-apply
# writes dropbear's args.
mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/etc/default"

# The root is a read-only squashfs. SSH host keys live on the persistent
# /var (dropbear.service.d/30-chumby.conf creates the directory).
rm -rf "${TARGET_DIR}/etc/dropbear"
ln -sfn /var/lib/dropbear "${TARGET_DIR}/etc/dropbear"
mkdir -p "${TARGET_DIR}/var/lib/chumby"

# (The /etc entries that point into /run/chumby-config are made in
# post-fakeroot.sh, on the image's copy: buildroot itself still writes
# /etc/hostname, /etc/timezone and /etc/shadow after this script.)
# systemd mounts a generated machine-id over an empty one on a ro root.
: > "${TARGET_DIR}/etc/machine-id"
# Nothing to mark: the image is replaced whole, never updated in place.
ln -sfn /dev/null "${TARGET_DIR}/etc/systemd/system/systemd-update-done.service"
# Where init-ram leaves the carrier (p2) after pivoting.
mkdir -p "${TARGET_DIR}/media/sd"

# MediaTek MT7610U USB WiFi (Panda PAU0A; driver mt76x0u): its firmware,
# which buildroot's linux-firmware has no option for (only the MT7610E's).
mkdir -p "${TARGET_DIR}/lib/firmware/mediatek"
cp "${BUILD_DIR}"/linux-firmware-*/mediatek/mt7610u.bin "${TARGET_DIR}/lib/firmware/mediatek/"

# systemd units replace the shared overlay's sysvinit scripts.
rm -f "${TARGET_DIR}"/etc/init.d/S[0-9]*

# chumby-stream uses libmpg123 and the fixed-point libfaad only: not the
# command-line players, out123, or faad2's floating-point/DRM builds.
rm -f "${TARGET_DIR}"/usr/bin/faad "${TARGET_DIR}"/usr/bin/mpg123* \
      "${TARGET_DIR}"/usr/bin/out123 "${TARGET_DIR}"/usr/lib/libout123.so* \
      "${TARGET_DIR}"/usr/lib/libfaad.so* "${TARGET_DIR}"/usr/lib/libfaad_drm.so*

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
