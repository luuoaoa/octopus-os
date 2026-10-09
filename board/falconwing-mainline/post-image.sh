#!/bin/bash
# Assemble sdcard.img: stock boot partition with our kernel in krnA, the
# ext4 carrier (early boot + rootfs.squashfs + persistent data) and the
# FAT config partition.
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

# p2, the "carrier": early boot (busybox + its libraries + /sbin/init-ram),
# the system image (rootfs.squashfs, copied into RAM at boot) and the
# persistent data (/data/var, bound onto /var). See carrier/sbin/init-ram.
CARRIER="${BUILD_DIR}/carrier"
rm -rf "${CARRIER}"
mkdir -p "${CARRIER}"/{bin,lib,dev,proc,ram,data/var}
cp -a "${BOARD_DIR}/carrier/." "${CARRIER}/"
cp "${TARGET_DIR}/bin/busybox" "${CARRIER}/bin/"
for applet in sh mount umount cp mkdir pivot_root chroot; do
	ln -sf busybox "${CARRIER}/bin/${applet}"
done

READELF=$(ls "${HOST_DIR}"/bin/*-readelf | head -n 1)
find_lib() {
	for d in lib usr/lib; do
		[ -e "${TARGET_DIR}/${d}/$1" ] && { echo "${TARGET_DIR}/${d}/$1"; return; }
	done
	echo "ERROR: $1 (needed by busybox) not in the target" >&2
	exit 1
}
copy_with_deps() {
	local f dep
	for dep in $("${READELF}" -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		[ -e "${CARRIER}/lib/${dep}" ] && continue
		f=$(find_lib "${dep}")
		cp -L "${f}" "${CARRIER}/lib/${dep}"
		copy_with_deps "${CARRIER}/lib/${dep}"
	done
}
copy_with_deps "${CARRIER}/bin/busybox"
INTERP=$("${READELF}" -l "${CARRIER}/bin/busybox" | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
cp -L "$(find_lib "$(basename "${INTERP}")")" "${CARRIER}${INTERP}"

cp "${BINARIES_DIR}/rootfs.squashfs" "${CARRIER}/rootfs.squashfs"
cp -a "${TARGET_DIR}/var/." "${CARRIER}/data/var/"
echo "carrier: rootfs.squashfs $(stat -c %s "${CARRIER}/rootfs.squashfs") bytes," \
	"early boot $(du -sk "${CARRIER}/bin" "${CARRIER}/lib" | awk '{s+=$1} END {print s}') KiB"

rm -f "${BINARIES_DIR}/carrier.ext4"
"${HOST_DIR}/sbin/mkfs.ext4" -q -F -L chumby -E root_owner=0:0 \
	-d "${CARRIER}" "${BINARIES_DIR}/carrier.ext4" 256M

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
