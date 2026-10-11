#!/bin/sh
# For maintainers: the SDK's two Docker images, from a Chumby build
# (bash docker-build.sh falconwing-mainline, then "make sdk" in it):
#
#   chumby-sdk   the toolchain (buildroot's SDK) + chumby-app + examples
#   chumby-sim   the device's root filesystem + the simulator's session
#
# App developers don't run this: they load the images (docker load) and
# use sdk/chumby-app.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
IMG=$TOP/output-falconwing-mainline/images
SDK_TAR=$IMG/arm-buildroot-linux-gnueabi_sdk-buildroot.tar.gz

[ -f "$SDK_TAR" ] || { echo "no $SDK_TAR: make sdk first" >&2; exit 1; }
[ -f "$IMG/rootfs.tar" ] || { echo "no $IMG/rootfs.tar: build the image first" >&2; exit 1; }

ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT

cp "$SDK_TAR" "$ctx/sdk.tar.gz"
cp "$TOP/sdk/Dockerfile" "$TOP/sdk/chumby-app" "$ctx/"
cp -r "$TOP/sdk/examples" "$ctx/"
docker build -t chumby-sdk "$ctx"

rm -rf "${ctx:?}"/*
cp "$IMG/rootfs.tar" "$TOP/sdk/sim/Dockerfile" "$TOP/sdk/sim/chumby-sim-session" \
	"$TOP/sdk/sim/asound.conf" "$ctx/"
docker build -t chumby-sim "$ctx"

echo "chumby-sdk, chumby-sim built. To hand them out:"
echo "  docker save chumby-sdk chumby-sim | zstd > chumby-sdk-images.tar.zst"
