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

# Stock FM radio tool + its glibc 2.8 runtime (/lib/ld-linux.so.3,
# /lib/libc.so.6). Nothing else on the image uses glibc. fmradio looks
# for /psp/fmradio_xclk to decide whether to drive the tuner's reference
# clock from PWM3, so keep a /psp directory around for that flag.
if [ ! -f "${STOCK}/rootfs/usr/bin/fmradio" ]; then
    echo "ERROR: ${STOCK} predates FM radio support; re-run extract-stock.sh" >&2
    exit 1
fi
install -m 0755 "${STOCK}/rootfs/usr/bin/fmradio" "${TARGET_DIR}/usr/bin/fmradio"
install -m 0755 "${STOCK}/rootfs/lib/ld-2.8.so" "${STOCK}/rootfs/lib/libc-2.8.so" "${TARGET_DIR}/lib/"
ln -sf ld-2.8.so "${TARGET_DIR}/lib/ld-linux.so.3"
ln -sf libc-2.8.so "${TARGET_DIR}/lib/libc.so.6"
mkdir -p "${TARGET_DIR}/psp"

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
