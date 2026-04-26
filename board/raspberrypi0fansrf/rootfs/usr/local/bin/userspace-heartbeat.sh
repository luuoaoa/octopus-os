#!/bin/sh
# Triple-blink the ACT LED in a tight loop.
# Pattern is intentionally distinct from the kernel's default "heartbeat"
# trigger (thump-thump-pause): three quick blinks, then 1.2s rest.
# If you see this pattern, multi-user.target was reached.
set -u

LED_DIR=
for d in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/PWR; do
    [ -e "$d/brightness" ] && LED_DIR="$d" && break
done
[ -n "$LED_DIR" ] || { echo "no ACT-class LED found" >&2; exit 1; }

echo none > "$LED_DIR/trigger" 2>/dev/null || true

while :; do
    for _ in 1 2 3; do
        echo 1 > "$LED_DIR/brightness"
        sleep 0.1
        echo 0 > "$LED_DIR/brightness"
        sleep 0.15
    done
    sleep 1.2
done
