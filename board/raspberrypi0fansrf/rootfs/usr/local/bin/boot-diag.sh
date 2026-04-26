#!/bin/sh
# Every boot, snapshot network/service state to /boot/boot-diag.txt.
# Pull the SD card and read it on any host (FAT, no ext4 driver needed).
# Last-resort fallback for when WiFi/SSH didn't come up.
set -u

OUT=/boot/boot-diag.txt

# /boot is mounted ro per fstab; remount rw briefly to write the diag.
mount -o remount,rw /boot 2>/dev/null || true

run() { echo "=== $* ==="; "$@" 2>&1; echo; }

{
    date
    echo "uptime: $(cat /proc/uptime)"
    echo
    run hostname
    run ip -br link
    run ip -br addr
    run ls /sys/class/net
    run mount
    run df -hT
    run ls /var
    run networkctl --no-pager
    run systemctl --no-pager is-active \
        pi-imager-apply.service create-var-partition.service \
        systemd-networkd.service wpa_supplicant@wlan0.service \
        dhcpcd.service dropbear.service userspace-heartbeat.service
    echo "=== /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ==="
    [ -r /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ] && \
        cat /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
    echo
    run systemctl --no-pager status -l --lines=40 pi-imager-apply.service
    run systemctl --no-pager status -l --lines=40 wpa_supplicant@wlan0.service
    run systemctl --no-pager status -l --lines=20 systemd-networkd.service
    run systemctl --no-pager status -l --lines=15 create-var-partition.service
    echo "=== dmesg ==="
    dmesg 2>&1
    echo
    run journalctl -b --no-pager -u pi-imager-apply.service
    run journalctl -b --no-pager -u wpa_supplicant@wlan0.service
    run journalctl -b --no-pager -u systemd-networkd.service
    run journalctl -b --no-pager -u create-var-partition.service
    echo "DONE"
} > "$OUT" 2>&1

sync
mount -o remount,ro /boot 2>/dev/null || true

# Settle, then signal "diag captured, safe to power off" via the ACT LED:
# stop the userspace heartbeat and leave the LED solid on.
sleep 5
systemctl stop userspace-heartbeat.service 2>/dev/null || true
sleep 1

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done
if [ -n "$LED_DIR" ]; then
    echo none > "$LED_DIR/trigger" 2>/dev/null || true
    echo 1 > "$LED_DIR/brightness" 2>/dev/null || true
fi
