#!/bin/bash
# Assemble sdcard.img: stock boot partition with our kernel in krnA, the
# ext4 rootfs and the FAT config partition.
set -euo pipefail

BOARD_DIR="$(dirname "$0")"
FW_DIR="${BOARD_DIR}/../falconwing"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
KERNEL="${BINARIES_DIR}/zImage.imx23-chumby-one"
BOOT_IMG="${BINARIES_DIR}/falconwing-boot.img"

if [ ! -f "${FW_DIR}/stock/boot.img" ]; then
    echo "ERROR: ${FW_DIR}/stock/boot.img missing; run board/falconwing/extract-stock.sh first" >&2
    exit 1
fi

# Find krnA in the config block table (Cfg* at 0xC000, 16-byte entries
# {offset, size, flags, name} from 0xC400) rather than hardcoding it.
read -r KRN_OFF KRN_SIZE < <(python3 - "${FW_DIR}/stock/boot.img" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[0xC000:0xC004] == b'Cfg*', 'no config block'
for i in range(0xC400, 0xC800, 16):
    off, size, _, name = struct.unpack('<III4s', d[i:i + 16])
    if off == 0xFFFFFFFF:
        break
    if name == b'krnA':
        print(off, size)
        break
else:
    sys.exit('krnA not found in config block')
PY
)

KSIZE=$(stat -c %s "${KERNEL}")
echo "krnA: offset=$(printf 0x%x "${KRN_OFF}") slot=${KRN_SIZE} kernel+dtb=${KSIZE}"
if [ "${KSIZE}" -gt "${KRN_SIZE}" ]; then
    echo "ERROR: zImage+DTB (${KSIZE} bytes) exceeds the krnA slot (${KRN_SIZE} bytes)." >&2
    echo "       Trim board/falconwing-mainline/linux.config.fragment." >&2
    exit 1
fi

# Stock boot partition with the slot zeroed, then our kernel in it. The
# bootloader loads the whole slot; the decompressor finds the DTB right
# after the zImage, so the zero padding is harmless.
cp "${FW_DIR}/stock/boot.img" "${BOOT_IMG}"
dd if=/dev/zero of="${BOOT_IMG}" bs=1 seek="${KRN_OFF}" count="${KRN_SIZE}" conv=notrunc status=none
dd if="${KERNEL}" of="${BOOT_IMG}" bs=1 seek="${KRN_OFF}" conv=notrunc status=none

rm -rf "${BINARIES_DIR}/config"
cp -r "${FW_DIR}/config-partition" "${BINARIES_DIR}/config"

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
