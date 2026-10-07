#!/bin/sh
# Runs against TARGET_DIR before the ext3 image is assembled.
set -eu

TARGET_DIR="$1"
BOARD_DIR="$(dirname "$0")"
STOCK="${BOARD_DIR}/stock"

if [ ! -d "${STOCK}/rootfs/lib/modules" ] || [ ! -f "${STOCK}/boot.img" ]; then
    echo "ERROR: ${STOCK} is missing. Extract the stock bits first (on the host):" >&2
    echo "  board/falconwing/extract-stock.sh /path/to/stock-sdcard.img" >&2
    exit 1
fi

# Kernel modules + wifi firmware from the stock image; they must match the
# stock 2.6.28-chumby kernel we boot.
mkdir -p "${TARGET_DIR}/lib/modules" "${TARGET_DIR}/lib/firmware"
rsync -a --delete "${STOCK}/rootfs/lib/modules/" "${TARGET_DIR}/lib/modules/"
rsync -a "${STOCK}/rootfs/lib/firmware/" "${TARGET_DIR}/lib/firmware/"

# Mountpoint for the FAT config partition, and where sdcard-config-apply
# writes dropbear's args (empty dirs don't survive in git, so not overlay).
mkdir -p "${TARGET_DIR}/boot" "${TARGET_DIR}/etc/default"

# The rootfs is rw ext3, so keep dropbear's host keys on it (buildroot
# points /etc/dropbear at tmpfs /var/run/dropbear by default).
if [ -L "${TARGET_DIR}/etc/dropbear" ]; then
    rm -f "${TARGET_DIR}/etc/dropbear"
fi
mkdir -p "${TARGET_DIR}/etc/dropbear"

# Build stamp, same idea as pifansrf's motd.
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

  falconwing (Chumby One) — buildroot userland on stock 2.6.28 kernel
  build:  ${COMMIT}${DIRTY}
  date:   $(date -u +"%Y-%m-%dT%H:%M:%SZ")

MOTD
