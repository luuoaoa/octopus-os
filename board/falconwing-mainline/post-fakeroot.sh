#!/bin/sh
# Runs inside fakeroot after buildroot's "systemctl preset-all", which
# would drop links to template instances made earlier: enable them here.
set -eu

TARGET_DIR="$1"
WANTS="${TARGET_DIR}/etc/systemd/system/multi-user.target.wants"

mkdir -p "${WANTS}"
ln -sf /usr/lib/systemd/system/wpa_supplicant@.service "${WANTS}/wpa_supplicant@wlan0.service"
ln -sf /usr/lib/systemd/system/udhcpc@.service "${WANTS}/udhcpc@wlan0.service"

# Read-only root: settings applied at boot from the FAT partition
# (sdcard-config-apply) land in /run/octopus-config; /etc points there.
# Done here, on the image's copy, after buildroot has finished writing
# /etc/hostname, /etc/timezone and (mkusers) /etc/shadow.
CFG=/run/octopus-config
mkdir -p "${TARGET_DIR}/etc/wpa_supplicant" "${TARGET_DIR}/etc/default" "${TARGET_DIR}/root/.ssh"
chmod 700 "${TARGET_DIR}/root/.ssh"
for link in etc/wpa_supplicant/wpa_supplicant-wlan0.conf:wpa_supplicant-wlan0.conf \
	    etc/hostname:hostname etc/localtime:localtime etc/timezone:timezone \
	    etc/default/dropbear:dropbear root/.ssh/authorized_keys:authorized_keys; do
    rm -f "${TARGET_DIR}/${link%%:*}"
    ln -s "${CFG}/${link#*:}" "${TARGET_DIR}/${link%%:*}"
done
# /etc/shadow: tmpfiles seeds the /run copy from shadow.default
# (usr/lib/tmpfiles.d/octopus.conf); the config script rewrites it when
# user-data sets a password.
if [ ! -L "${TARGET_DIR}/etc/shadow" ]; then
    mv "${TARGET_DIR}/etc/shadow" "${TARGET_DIR}/etc/shadow.default"
    ln -s "${CFG}/shadow" "${TARGET_DIR}/etc/shadow"
fi
