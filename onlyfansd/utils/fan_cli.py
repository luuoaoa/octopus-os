#!/usr/bin/env python3
"""Minimal CLI wrapper around control_fan.py — use it to send a single RF command.

Reads fan IDs from ../config.yaml (same format as the Rust daemon's config).
Requires `sendook` on PATH and a room defined under `fans:` in the config.

Usage:
    ./fan_cli.py <room> <command>

Examples:
    ./fan_cli.py living_room light
    ./fan_cli.py upstairs speed3
    ./fan_cli.py baby_room stop

Commands: reverse, light, stop, speed1..speed6, pair.
"""
import sys
from control_fan import control_fan, CMDS, FAN_IDS


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    room, cmd = sys.argv[1], sys.argv[2]
    if room not in FAN_IDS:
        sys.exit(f"Unknown room: {room!r}. Known rooms: {sorted(FAN_IDS)}")
    if cmd not in CMDS:
        sys.exit(f"Unknown command: {cmd!r}. Known commands: {sorted(CMDS)}")
    control_fan(room, cmd)


if __name__ == "__main__":
    main()
