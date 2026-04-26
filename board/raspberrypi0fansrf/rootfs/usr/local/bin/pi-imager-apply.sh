#!/bin/sh
# Read /boot/{network-config,user-data} (Pi Imager-style YAML) and apply
# WiFi creds, hostname, timezone, and SSH on/off. Idempotent.
#
# Files we honour, fields we read:
#   /boot/network-config:  access-points (SSID + password [hex] + hidden)
#   /boot/user-data:       hostname, timezone, ssh_pwauth
# Everything else is ignored.
#
# Logs to stderr (-> journalctl -u pi-imager-apply) for debuggability.
set -u

log() { echo "pi-imager-apply: $*" >&2; }

# Strip leading/trailing whitespace and surrounding quotes from a value.
strip() {
    s=${1#"${1%%[![:space:]]*}"}
    s=${s%"${s##*[![:space:]]}"}
    s=${s#\"}; s=${s%\"}
    s=${s#\'}; s=${s%\'}
    printf '%s' "$s"
}

BOOT=/boot
NET="$BOOT/network-config"
USR="$BOOT/user-data"

# Dump what's actually on /boot so missing-file mysteries are diagnosable.
log "/boot listing:"
ls -la "$BOOT" 2>&1 | while IFS= read -r line; do log "  $line"; done

# === network-config -> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ===
if [ -f "$NET" ]; then
    if [ ! -r "$NET" ]; then
        log "WARN: $NET exists but is not readable"
    fi
    log "reading $NET"

    # SSID: the first quoted key under `access-points:`.
    SSID_RAW=$(sed -nE '/^[[:space:]]+access-points:/,$ {
        s/^[[:space:]]+"([^"]+)":[[:space:]]*$/\1/p
    }' "$NET" | head -1)
    SSID=$(strip "$SSID_RAW")

    # PSK (hex or passphrase) inside the first quoted password field.
    PSK_RAW=$(sed -nE 's/^[[:space:]]+password:[[:space:]]+"?([^"]+)"?[[:space:]]*$/\1/p' "$NET" | head -1)
    PSK=$(strip "$PSK_RAW")

    # Hidden flag.
    HIDDEN=
    grep -qE '^[[:space:]]+hidden:[[:space:]]+true[[:space:]]*$' "$NET" && HIDDEN=1

    log "parsed SSID=[$SSID] psk_len=${#PSK} hidden=${HIDDEN:-0}"

    if [ -n "$SSID" ] && [ -n "$PSK" ]; then
        # Decide quoting: 64 hex chars => raw hex PSK (no quotes); otherwise
        # an ASCII passphrase (must be quoted).
        case "$PSK" in
            *[!0-9a-fA-F]*) PSK_LINE="    psk=\"$PSK\"" ;;
            *)
                if [ ${#PSK} -eq 64 ]; then
                    PSK_LINE="    psk=$PSK"
                else
                    PSK_LINE="    psk=\"$PSK\""
                fi
                ;;
        esac

        mkdir -p /etc/wpa_supplicant
        {
            echo "ctrl_interface=/var/run/wpa_supplicant"
            echo "update_config=1"
            echo "country=US"
            echo
            echo "network={"
            echo "    ssid=\"$SSID\""
            echo "    key_mgmt=WPA-PSK"
            echo "$PSK_LINE"
            [ -n "$HIDDEN" ] && echo "    scan_ssid=1"
            echo "}"
        } > /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
        chmod 600 /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
        log "wrote /etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
    else
        log "WARN: SSID or PSK missing; skipping wpa_supplicant config"
    fi
else
    log "no $NET; skipping WiFi config"
fi

# === user-data -> hostname, timezone, ssh_pwauth ===
if [ -f "$USR" ]; then
    log "reading $USR"

    HOST=$(sed -nE 's/^hostname:[[:space:]]+(.*)[[:space:]]*$/\1/p' "$USR" | head -1)
    HOST=$(strip "$HOST")
    if [ -n "$HOST" ]; then
        echo "$HOST" > /etc/hostname
        hostname "$HOST" 2>/dev/null || true
        log "hostname=$HOST"
    fi

    TZ=$(sed -nE 's/^timezone:[[:space:]]+(.*)[[:space:]]*$/\1/p' "$USR" | head -1)
    TZ=$(strip "$TZ")
    if [ -n "$TZ" ] && [ -f "/usr/share/zoneinfo/$TZ" ]; then
        ln -sf "/usr/share/zoneinfo/$TZ" /etc/localtime
        log "timezone=$TZ"
    fi

    # Dropbear is disabled by default (see 30-pifansrf.preset). Only start
    # if user-data explicitly opts in with `ssh_pwauth: true`.
    SSH_PWAUTH=$(sed -nE 's/^ssh_pwauth:[[:space:]]+(.*)[[:space:]]*$/\1/p' "$USR" | head -1)
    SSH_PWAUTH=$(strip "$SSH_PWAUTH")
    case "$SSH_PWAUTH" in
        true|True|TRUE)
            # --no-block: just queue it. systemctl start without --no-block
            # waits for the unit to reach 'active', and dropbear's
            # After=network.target chain ultimately depends on us finishing
            # — that's a deadlock.
            systemctl start --no-block dropbear.service 2>/dev/null || true
            log "SSH queued (ssh_pwauth: true)"
            ;;
    esac
else
    log "no $USR; skipping hostname/timezone/ssh"
fi

exit 0
