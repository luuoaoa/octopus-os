#!/bin/sh
# Slow health blink: a brief flash every ~10 seconds. Visible enough to
# confirm userspace is still alive, quiet enough to ignore.
set -u

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done
[ -n "$LED_DIR" ] || { echo "no ACT-class LED found" >&2; exit 1; }

echo none > "$LED_DIR/trigger" 2>/dev/null || true

while :; do
    echo 1 > "$LED_DIR/brightness"
    sleep 0.1
    echo 0 > "$LED_DIR/brightness"
    sleep 9.9
done
