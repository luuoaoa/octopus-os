#!/bin/sh
# After boot, capture network + service state to /boot/boot-diag.txt on the
# FAT partition, then signal "diag done, safe to power off" by stopping the
# heartbeat service and setting the ACT LED solid on.
set -u

DIAG_MNT=/run/diag-boot
mkdir -p "${DIAG_MNT}"
mount -o sync -t vfat /dev/mmcblk0p1 "${DIAG_MNT}" 2>/dev/null || {
    echo "boot-diag: failed to mount /dev/mmcblk0p1" >&2
    exit 1
}

OUT="${DIAG_MNT}/boot-diag.txt"

run() { echo "=== $* ==="; "$@" 2>&1; echo; }

{
    date
    echo "uptime: $(cat /proc/uptime)"
    echo
    run ip -br link
    run ip -br addr
    run ls /sys/class/net
    run iw dev
    run iw reg get
    run rfkill list
    run lsmod
    run networkctl --no-pager
    run systemctl --no-pager is-active wpa_supplicant@wlan0.service systemd-networkd.service dhcpcd.service dropbear.service
    run systemctl --no-pager status -l --lines=40 wpa_supplicant@wlan0.service
    run systemctl --no-pager status -l --lines=20 systemd-networkd.service
    echo "=== dmesg (full) ==="
    dmesg 2>&1
    echo
    run journalctl -b --no-pager -u wpa_supplicant@wlan0.service
    run journalctl -b --no-pager -u systemd-networkd.service
    run ls /lib/firmware/brcm
    echo "DONE"
} > "${OUT}" 2>&1

sync
umount "${DIAG_MNT}" 2>/dev/null
sync

# Settle — let any in-flight IO drain
sleep 5

# Stop the triple-blink heartbeat and set the ACT LED solid on.
# Visual contract: solid on = "diag captured, safe to pull power".
systemctl stop userspace-heartbeat.service 2>/dev/null

# Brief pause so the heartbeat script has exited before we grab the LED
sleep 1

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done
if [ -n "$LED_DIR" ]; then
    echo none > "$LED_DIR/trigger" 2>/dev/null || true
    echo 1 > "$LED_DIR/brightness" 2>/dev/null || true
fi
