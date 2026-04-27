#!/bin/sh
# ACT LED heartbeat with two modes, switched via a flag file:
#   * Boot-diag pending (default): fast 0.5 on / 0.5 off — "userspace alive,
#     boot-diag still running, don't power off yet."
#   * Boot-diag done (/run/boot-diag-done exists): slow 0.5 on / 5 off —
#     "diag complete, system is idle and healthy, safe to power off."
# boot-diag.sh creates the flag in its EXIT trap, so the mode switch lands
# whether the diag dump succeeded or got SIGTERMed past its trap.
set -u

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done
[ -n "$LED_DIR" ] || { echo "no ACT-class LED found" >&2; exit 1; }

echo none > "$LED_DIR/trigger" 2>/dev/null || true

while :; do
    echo 1 > "$LED_DIR/brightness"
    sleep 0.5
    echo 0 > "$LED_DIR/brightness"
    if [ -e /run/boot-diag-done ]; then
        sleep 5
    else
        sleep 0.5
    fi
done
