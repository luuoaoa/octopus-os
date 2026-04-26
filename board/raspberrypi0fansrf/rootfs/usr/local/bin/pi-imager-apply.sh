#!/bin/sh
# Read /boot/{network-config,user-data} (Pi Imager-style YAML) and apply
# WiFi creds, hostname, timezone, and SSH on/off. Idempotent.
#
# Files we honour, fields we read:
#   /boot/network-config:  access-points (SSID + password [hex] + hidden)
#   /boot/user-data:       hostname, timezone, ssh_pwauth
# Everything else is ignored.
set -u

BOOT=/boot
NET="$BOOT/network-config"
USR="$BOOT/user-data"

# === network-config -> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ===
if [ -r "$NET" ]; then
    SSID=$(awk '
        /^[[:space:]]*access-points:/ { in_ap = 1; next }
        in_ap && /^[[:space:]]+"[^"]+":/ {
            line = $0
            sub(/^[[:space:]]+"/, "", line)
            sub(/":[[:space:]]*$/, "", line)
            print line; exit
        }' "$NET")
    PSK=$(awk -F'"' '/^[[:space:]]+password:/{print $2; exit}' "$NET")
    HIDDEN=$(awk '/^[[:space:]]+hidden:[[:space:]]+true/{print 1; exit}' "$NET")

    if [ -n "$SSID" ] && [ -n "$PSK" ]; then
        mkdir -p /etc/wpa_supplicant
        {
            echo "ctrl_interface=/var/run/wpa_supplicant"
            echo "update_config=1"
            echo "country=US"
            echo
            echo "network={"
            echo "    ssid=\"$SSID\""
            echo "    psk=$PSK"
            [ -n "$HIDDEN" ] && echo "    scan_ssid=1"
            echo "}"
        } > /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
        chmod 600 /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
        echo "pi-imager-apply: WiFi configured (SSID='$SSID' hidden=${HIDDEN:-0})"
    fi
fi

# === user-data -> hostname, timezone, ssh_pwauth ===
if [ -r "$USR" ]; then
    HOST=$(awk '/^hostname:[[:space:]]+/{print $2; exit}' "$USR")
    if [ -n "$HOST" ]; then
        echo "$HOST" > /etc/hostname
        hostname "$HOST" 2>/dev/null || true
        echo "pi-imager-apply: hostname=$HOST"
    fi

    TZ=$(awk '/^timezone:[[:space:]]+/{print $2; exit}' "$USR")
    if [ -n "$TZ" ] && [ -f "/usr/share/zoneinfo/$TZ" ]; then
        ln -sf "/usr/share/zoneinfo/$TZ" /etc/localtime
        echo "pi-imager-apply: timezone=$TZ"
    fi

    SSH_PWAUTH=$(awk '/^ssh_pwauth:[[:space:]]+/{print $2; exit}' "$USR")
    case "$SSH_PWAUTH" in
        false|False|FALSE)
            systemctl stop dropbear.service 2>/dev/null || true
            systemctl disable dropbear.service 2>/dev/null || true
            echo "pi-imager-apply: SSH disabled (ssh_pwauth: false)"
            ;;
        # true / absent / anything else: leave default (dropbear enabled)
    esac
fi

exit 0
