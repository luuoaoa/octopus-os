#!/bin/sh
# Snapshot boot state to /boot/boot-diag.txt. Triggered by boot-diag.timer
# ~30s after boot. Always finishes by tripping /run/boot-diag-done, which
# tells userspace-heartbeat to switch from fast (0.5/0.5) to slow (0.5/5)
# blink — visible "diag complete" signal that survives even if this dump
# fails halfway.
#
# Defensive choices (each addresses a previously-observed silent failure):
#   * If /boot can't be remounted rw, we'd lose the entire diag to a silent
#     redirect failure (heartbeat slow but no file). Fall back to /run.
#   * Hang-prone commands (journalctl, systemctl, networkctl) wrapped in
#     `timeout`, since a flaky /var can wedge journald and stall us past
#     the systemd TimeoutStartSec → SIGKILL → no FAT flush → empty file.
#   * Cheap reliable info written + synced first, so even a later SIGKILL
#     leaves something on the FAT.
set -u

OUT=/boot/boot-diag.txt

signal_done() {
    sync
    mount -o remount,ro /boot 2>/dev/null || true
    : > /run/boot-diag-done 2>/dev/null || true
}
trap signal_done EXIT INT TERM

mount -o remount,rw /boot 2>/dev/null || true
if ! : > "$OUT" 2>/dev/null; then
    # /boot is read-only or unmounted. Stash the diag in /run so it's at
    # least available via SSH if the Pi does come up; on the FAT we won't
    # see it, but at least we won't have wasted the run.
    OUT=/run/boot-diag.txt
    : > "$OUT" 2>/dev/null || exit 0
fi

run()  { echo "=== $* ==="; "$@" 2>&1; echo; }
trun() { echo "=== $* (timeout 10s) ==="; timeout 10 "$@" 2>&1; echo "(rc=$?)"; echo; }

# Phase 1: cheap, reliable info + /var integrity probe. Synced before we
# touch anything that might wedge.
{
    date
    echo "out: $OUT"
    echo "uptime: $(cat /proc/uptime)"
    echo
    run hostname
    run ip link
    run ip addr
    run ls /sys/class/net
    run ls -la /boot
    run mount
    run cat /proc/mounts
    run df -hT

    echo "=== /var partition status ==="
    if [ -b /dev/mmcblk0p2 ]; then
        echo "/dev/mmcblk0p2: present"
        run blkid /dev/mmcblk0p2
        if grep -qE '^[^ ]+ /var ' /proc/mounts; then
            echo "/var: mounted"
            run df -hT /var
            run ls -la /var
            # Confirm /var isn't silently mounted ro after fs errors.
            if (echo ok > /var/.boot-diag-rw-test) 2>/dev/null; then
                echo "/var: writable"
                rm -f /var/.boot-diag-rw-test
            else
                echo "/var: mounted READ-ONLY (fs likely flipped ro on errors)"
            fi
        else
            echo "/var: NOT MOUNTED — probing /dev/mmcblk0p2 read-only:"
            mkdir -p /run/var-probe
            trun mount -t ext4 -o ro /dev/mmcblk0p2 /run/var-probe
            umount /run/var-probe 2>/dev/null || true
            rmdir /run/var-probe 2>/dev/null || true
        fi
    else
        echo "/dev/mmcblk0p2: MISSING (create-var-partition.service didn't run or failed)"
    fi
    echo
} >> "$OUT" 2>&1
sync

# Phase 2: dmesg + everything that talks to systemd/journald/networkd.
# A wedged /var can stall any of these — that's why they all get timeouts.
{
    echo "=== dmesg ==="
    dmesg 2>&1
    echo
    trun networkctl --no-pager
    trun systemctl --no-pager is-active \
        pi-imager-apply.service create-var-partition.service \
        systemd-networkd.service wpa_supplicant@wlan0.service \
        dhcpcd.service dropbear.service userspace-heartbeat.service
    echo "=== /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ==="
    [ -r /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ] && \
        cat /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
    echo
    trun systemctl --no-pager status -l --lines=40 pi-imager-apply.service
    trun systemctl --no-pager status -l --lines=40 wpa_supplicant@wlan0.service
    trun systemctl --no-pager status -l --lines=20 systemd-networkd.service
    trun systemctl --no-pager status -l --lines=15 create-var-partition.service
    trun journalctl -b --no-pager -u pi-imager-apply.service
    trun journalctl -b --no-pager -u wpa_supplicant@wlan0.service
    trun journalctl -b --no-pager -u systemd-networkd.service
    trun journalctl -b --no-pager -u create-var-partition.service
    echo "DONE"
} >> "$OUT" 2>&1
sync
