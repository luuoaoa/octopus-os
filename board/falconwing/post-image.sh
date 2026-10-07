#!/bin/bash
# Assemble sdcard.img: stock boot partition + our ext3 rootfs + FAT config.
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BOARD_DIR}/genimage-falconwing.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

cp "${BOARD_DIR}/stock/boot.img" "${BINARIES_DIR}/falconwing-boot.img"

rm -rf "${BINARIES_DIR}/config"
cp -r "${BOARD_DIR}/config-partition" "${BINARIES_DIR}/config"

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
