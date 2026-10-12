#!/bin/bash
# Build an image inside a pinned Ubuntu 20.04 container.
# Mounts the repository (with the buildroot subtree inside) at /work.
#
#   bash docker-build.sh          # Chumby One   -> output-falconwing/
#   bash docker-build.sh rpi64    # Raspberry Pi -> output-rpi64/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=octopus-builder:ubuntu20.04
BOARD="${1:-falconwing}"

[ -f "${SCRIPT_DIR}/build-${BOARD}.sh" ] || { echo "no build-${BOARD}.sh" >&2; exit 1; }

docker build \
    --build-arg UID="$(id -u)" \
    --build-arg GID="$(id -g)" \
    -t "${IMAGE}" \
    "${SCRIPT_DIR}"

# Rootless Docker: the container's root is you (its other users map to
# sub-ids that can't write here), so build as root in it; some packages'
# configure refuses root unless told it's fine.
AS_BUILDER=
if docker info -f '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    AS_BUILDER="--user 0:0 -e FORCE_UNSAFE_CONFIGURE=1"
fi

# shellcheck disable=SC2086
docker run --rm ${AS_BUILDER} \
    -v "${SCRIPT_DIR}:/work" \
    -w /work \
    "${IMAGE}" \
    bash "build-${BOARD}.sh"
