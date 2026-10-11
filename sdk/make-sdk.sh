#!/bin/bash
# make-sdk.sh [BOARD]: build the Chumby SDK for a board, from source
# (docs/sdk-build.md). BOARD is a configs/<BOARD>_defconfig with the
# Chumby's UI in it; the default, raspad, is any 64-bit Raspberry Pi
# (aarch64). falconwing_mainline is the Chumby One itself (ARMv5).
#
# In the pinned build container (as docker-build.sh): the board's image,
# then buildroot's SDK (the toolchain and the sysroot, relocatable). Then
# two Docker images, tagged with the board:
#
#   chumby-sdk:BOARD   the SDK + chumby-app + the examples (x86-64)
#   chumby-sim:BOARD   the board's root filesystem, to run its UI on the PC
#
# The first build takes an hour or two (everything from source); later
# ones only what changed. Use the images with CHUMBY_BOARD=BOARD
# chumby-app ... (raspad is chumby-app's default too).
set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${1:-raspad}"
DEFCONFIG="${BOARD}_defconfig"
OUT="output-${BOARD//_/-}"
BUILDER=pifansrf-builder:ubuntu20.04

[ -f "$TOP/configs/$DEFCONFIG" ] || { echo "no configs/$DEFCONFIG" >&2; exit 1; }
grep -q '^BR2_PACKAGE_LIBCHUMBY=y' "$TOP/configs/$DEFCONFIG" ||
	echo "warning: $DEFCONFIG has no libchumby: no SDK for Chumby apps" >&2
grep -q '^BR2_TARGET_ROOTFS_TAR=y' "$TOP/configs/$DEFCONFIG" ||
	echo "warning: $DEFCONFIG makes no rootfs.tar: no simulator image" >&2

echo "== the build container"
docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$BUILDER" "$TOP"

echo "== $BOARD: the image and the SDK (in $OUT/)"
docker run --rm -v "$TOP:/work" -w /work/buildroot "$BUILDER" bash -c "
	set -e
	make O=/work/$OUT BR2_EXTERNAL=/work $DEFCONFIG
	make O=/work/$OUT
	make O=/work/$OUT sdk"

exec sh "$TOP/sdk/make-images.sh" "$BOARD"
