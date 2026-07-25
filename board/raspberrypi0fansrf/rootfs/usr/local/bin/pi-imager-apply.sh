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

# Find first existing file matching any of the globs/paths in $@.
find_first() {
    for p in "$@"; do
        [ -f "$p" ] && { printf '%s\n' "$p"; return 0; }
    done
}

# Dump what's actually on /boot so missing-file mysteries are diagnosable.
log "/boot listing:"
ls -la "$BOOT" 2>&1 | while IFS= read -r line; do log "  $line"; done

# FAT 8.3 truncation: if a file's LFN entry is missing, /boot/network-config
# is invisible and only shows up under its short name (e.g. network-.con,
# NETWOR~1, etc.). Match by glob so we find whichever spelling exists.
NET=$(find_first "$BOOT/network-config" "$BOOT"/network-* "$BOOT"/NETWORK-* "$BOOT"/NETWOR~*)
USR=$(find_first "$BOOT/user-data"      "$BOOT"/user-data* "$BOOT"/USER-DAT*  "$BOOT"/USER-D~*)
[ -n "$NET" ] && [ "$NET" != "$BOOT/network-config" ] && \
    log "NOTE: network-config matched as '$NET' (FAT short name?). Re-save the file from a tool that writes LFN entries to fix."
[ -n "$USR" ] && [ "$USR" != "$BOOT/user-data" ] && \
    log "NOTE: user-data matched as '$USR'"

# === network-config -> /etc/wpa_supplicant/wpa_supplicant-wlan0.conf ===
if [ -n "${NET:-}" ] && [ -f "$NET" ]; then
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
if [ -n "${USR:-}" ] && [ -f "$USR" ]; then
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

    # Root password: the first `passwd:` value (Pi Imager writes it under a
    # users: entry). It's already a crypt(3) hash -- $id$salt$hash, typically
    # $5$ (SHA-256) or $6$ (SHA-512) -- so it goes into /etc/shadow verbatim;
    # there's nothing to encode here. The image has no `pi` user, so whichever
    # user block it came from, we apply it to root.
    #
    # /etc is tmpfs, so this is re-done every boot and reverts if the file goes
    # away. Guard hard on the $id$salt$hash shape: a malformed field in
    # /etc/shadow locks root out of a device we can only reach over the tunnel.
    PW=$(sed -nE 's/^[[:space:]]*passwd:[[:space:]]+(\$[0-9a-zA-Z]+\$[^[:space:]]+)[[:space:]]*$/\1/p' "$USR" | head -1)
    if [ -n "$PW" ]; then
        case "$PW" in
            *\$*\$*\$*)
                # awk, not sed: the hash is full of / and . and $, and only
                # field 2 of the root line may change.
                if awk -F: -v OFS=: -v h="$PW" \
                       '$1=="root"{$2=h} 1' /etc/shadow > /etc/shadow.new \
                   && [ -s /etc/shadow.new ]; then
                    chmod 600 /etc/shadow.new
                    mv /etc/shadow.new /etc/shadow
                    pwid=${PW#\$}; pwid=${pwid%%\$*}
                    log "root password set from $USR (crypt id \$$pwid\$, ${#PW} chars)"
                else
                    rm -f /etc/shadow.new
                    log "WARN: failed to rewrite /etc/shadow; keeping built-in password"
                fi
                ;;
            *)
                log "WARN: passwd: field is not a \$id\$salt\$hash crypt string; ignoring"
                ;;
        esac
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
