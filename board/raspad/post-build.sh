#!/bin/sh
# Runs after board/falconwing-mainline/post-build.sh (shared with the
# Chumby: dropbear keys on /var, the config links, no sysvinit scripts).
set -eu

TARGET_DIR="$1"

# No FM tuner (the Chumby's QN8005) in a RasPad.
rm -f "${TARGET_DIR}/usr/bin/chumby-fm" "${TARGET_DIR}/usr/bin/radio" \
      "${TARGET_DIR}/usr/bin/chumby-station-art" \
      "${TARGET_DIR}/usr/share/chumby/apps/fm.app" \
      "${TARGET_DIR}/usr/share/chumby/audio.d/fm"
# Nor the Chumby's resistive touchscreen.
rm -f "${TARGET_DIR}/etc/udev/rules.d/90-chumby-touch.rules"

# A read-write ext4 root here: nothing mounted over /media/sd.
rmdir "${TARGET_DIR}/media/sd" 2>/dev/null || true

GIT_DIR="${BR2_EXTERNAL_PIFANSRF_PATH:-.}"
# (In a container a worktree has no usable .git: the caller may pass it.)
COMMIT=${PIFANSRF_COMMIT:-$(git -C "${GIT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)}
cat > "${TARGET_DIR}/etc/motd" <<MOTD

  raspad (64-bit Raspberry Pi, Chumby UI) — buildroot
  build:  ${COMMIT}
  date:   $(date -u +"%Y-%m-%dT%H:%M:%SZ")

MOTD
