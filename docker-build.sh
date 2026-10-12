#!/bin/bash
# Build an image inside a pinned Ubuntu 20.04 container.
# Mounts the pifansrf dir (with the buildroot subtree inside) at /work.
#
#   bash docker-build.sh                      # Chumby One -> output-falconwing-mainline/
#   bash docker-build.sh raspad               # Raspberry Pi -> output-raspad/
#   bash docker-build.sh falconwing           # Chumby One, stock kernel -> output-falconwing/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=pifansrf-builder:ubuntu20.04
BOARD="${1:-falconwing-mainline}"

[ -f "${SCRIPT_DIR}/build-${BOARD}.sh" ] || { echo "no build-${BOARD}.sh" >&2; exit 1; }

docker build \
    --build-arg UID="$(id -u)" \
    --build-arg GID="$(id -g)" \
    -t "${IMAGE}" \
    "${SCRIPT_DIR}"

docker run --rm \
    -v "${SCRIPT_DIR}:/work" \
    -w /work \
    "${IMAGE}" \
    bash "build-${BOARD}.sh"
