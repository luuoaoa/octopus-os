#!/bin/sh
# Runs against TARGET_DIR before fakeroot/preset-all. Anything systemd-related
# placed here will be touched by preset-all afterwards; for service enables
# that need to survive preset-all, use post-fakeroot.sh instead.
set -u
set -e
