#!/bin/sh
# Runs against TARGET_DIR before fakeroot/preset-all. Anything systemd-related
# placed here will be touched by preset-all afterwards; for service enables
# that need to survive preset-all, use post-fakeroot.sh instead.
set -u
set -e

TARGET_DIR="$1"

# Stamp /etc/motd with the pifansrf git commit + build time so a login tells
# you which image you booted. Falls back to "unknown" if the build is run
# from a tarball with no .git.
GIT_DIR="${BR2_EXTERNAL_PIFANSRF_PATH:-.}"
COMMIT=unknown
DIRTY=
if git -C "${GIT_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
    COMMIT=$(git -C "${GIT_DIR}" rev-parse --short HEAD)
    if ! git -C "${GIT_DIR}" diff --quiet 2>/dev/null \
       || ! git -C "${GIT_DIR}" diff --cached --quiet 2>/dev/null; then
        DIRTY="-dirty"
    fi
fi
BUILD_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > "${TARGET_DIR}/etc/motd" <<EOF

  pifansrf — HomeKit/HA bridge (Casa Vieja TR301A via rpitx)
  build:  ${COMMIT}${DIRTY}
  date:   ${BUILD_TIME}

EOF

# Dropbear runs the user's shell directly and skips login(1), so /etc/motd
# is not printed on SSH login. Print it from /etc/profile.d when we detect
# an SSH connection — avoids duplicating it on the serial-console path,
# where busybox login already cats /etc/motd.
mkdir -p "${TARGET_DIR}/etc/profile.d"
cat > "${TARGET_DIR}/etc/profile.d/01-pifansrf-motd.sh" <<'EOF'
if [ -n "${SSH_CONNECTION:-}" ] && [ -f /etc/motd ]; then
    cat /etc/motd
fi
EOF

# Trim binaries that have no buildroot Kconfig knob to disable individually:
#   bootctl   — EFI bootloader management; the Pi uses the GPU bootloader.
#   tune2fs   — ext fs maintenance; we keep e2fsck for systemd-fsck on /var.
#   xz CLI    — liblzma stays (kmod can read xz-compressed modules) but the
#               command-line tools have no consumer on this device.
rm -f "${TARGET_DIR}/usr/bin/bootctl" \
      "${TARGET_DIR}/usr/sbin/tune2fs" \
      "${TARGET_DIR}/usr/bin/xz" \
      "${TARGET_DIR}/usr/bin/xzcat" \
      "${TARGET_DIR}/usr/bin/xzdec" \
      "${TARGET_DIR}/usr/bin/lzcat" \
      "${TARGET_DIR}/usr/bin/lzma" \
      "${TARGET_DIR}/usr/bin/lzmadec" \
      "${TARGET_DIR}/usr/bin/lzmainfo"

# Repoint /etc/dropbear at the persistent ext4 partition. Buildroot's
# dropbear package symlinks /etc/dropbear -> /var/run/dropbear (tmpfs),
# which causes host keys to be regenerated on every boot — defeats
# known_hosts and trips bin/update-pi.sh into needing sshpass. The
# shipped dropbear.service ExecStartPre tries `rm -f /etc/dropbear`
# first; on a read-only rootfs that fails and it falls through to
# `mkdir -p $(readlink /etc/dropbear)`, so the keys land on /var.
ln -sfn /var/lib/dropbear "${TARGET_DIR}/etc/dropbear"
