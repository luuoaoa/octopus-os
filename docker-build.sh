#!/bin/bash
# Build the pifansrf image inside a pinned Ubuntu 20.04 container.
# Mounts the parent develop/ dir so buildroot/ (sibling) is reachable.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEV_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE=pifansrf-builder:ubuntu20.04

docker build \
    --build-arg UID="$(id -u)" \
    --build-arg GID="$(id -g)" \
    -t "${IMAGE}" \
    "${SCRIPT_DIR}"

docker run --rm \
    -v "${DEV_DIR}:/work" \
    -w /work/pifansrf \
    "${IMAGE}" \
    bash build-pifansrf.sh
