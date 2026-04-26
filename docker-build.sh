#!/bin/bash
# Build the pifansrf image inside a pinned Ubuntu 20.04 container.
# Mounts the pifansrf dir (with the buildroot subtree inside) at /work.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=pifansrf-builder:ubuntu20.04

docker build \
    --build-arg UID="$(id -u)" \
    --build-arg GID="$(id -g)" \
    -t "${IMAGE}" \
    "${SCRIPT_DIR}"

docker run --rm \
    -v "${SCRIPT_DIR}:/work" \
    -w /work \
    "${IMAGE}" \
    bash build-pifansrf.sh
