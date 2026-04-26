# utils/ — Python reference implementation

This directory is kept as a **reference implementation** of the RF protocol
and as a set of small, independent tools. The production daemon lives in the
top-level `src/` (Rust).

Nothing here is required at runtime — it's here to illustrate the protocol
and to help debug new fans.

## Files

- **`control_fan.py`** — the protocol, end to end. Packs a `FAN_ID + command`
  into a 25-bit OOK frame and shells out to `sendook`. Read this first if you
  want to understand how the fans are driven.
- **`fan_cli.py`** — minimal CLI wrapper to test `control_fan.py` from the
  shell. `./fan_cli.py living_room light` sends one frame and exits. Useful
  when bringing up a new fan or verifying the radio path without running
  the daemon.
- **`decode_fan_remote.py`** — the other direction: decodes a recorded OOK
  capture from the physical remote into the raw bits, so you can work out
  the `FAN_ID` of a new fan.
- **`generate_esphome_config.py`** — emits an ESPHome YAML for an ESP32
  + CC1101 board that mirrors this behaviour. Not used by the daemon, but
  handy if you want to drive fans from an ESPHome node instead of a Pi.
- **`homekit_bridge.py`** — the old pyhap-based HomeKit bridge. Superseded
  by the Rust daemon's native HAP support (`src/homekit.rs`), but kept
  because it's a short, readable example of a combined Fan + Lightbulb
  accessory.

## config.yaml

`control_fan.py` reads `config.yaml` relative to the current working
directory. If you run `fan_cli.py` from the repo root with a `config.yaml`
there, it will pick up the same fan IDs the daemon uses:

```yaml
fans:
  living_room:  '00011001111000'
  upstairs:     '01111011000010'
  # ...
```
