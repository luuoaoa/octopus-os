#!/bin/sh
# Snapshot boot state to /boot/boot-diag.txt. Triggered by boot-diag.timer
# ~30s after boot. Always finishes by stopping the userspace heartbeat and
# leaving the ACT LED solid on (signal: "diag done, safe to power off"),
# even if the diag dump itself fails halfway.
set -u

OUT=/boot/boot-diag.txt

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done

signal_done() {
    sync
    mount -o remount,ro /boot 2>/dev/null || true
    systemctl stop userspace-heartbeat.service 2>/dev/null || true
    if [ -n "$LED_DIR" ]; then
        echo none > "$LED_DIR/trigger" 2>/dev/null || true
        echo 1    > "$LED_DIR/brightness" 2>/dev/null || true
    fi
}
trap signal_done EXIT INT TERM

mount -o remount,rw /boot 2>/dev/null || true

run() { echo "=== $* ==="; "$@" 2>&1; echo; }

{
    date
    echo "uptime: $(cat /proc/uptime)"
    echo
    run hostname
    run ip link
    run ip addr
    run ls /sys/class/net
    run ls -la /boot
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
