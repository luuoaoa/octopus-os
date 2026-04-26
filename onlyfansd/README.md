This is a daemon that exposes fan and light controls for Casa Vieja
ceiling fans (remote model `TR301A`) to Home Assistant and HomeKit. The
remote has a big SET button but no DIP switches, so we pair by sniffing
the remote's unique 14-bit `FAN_ID` off the air and replaying it.

Pairing a fan with an arbitrary ID
--

Per the remote's manual, the fan learns whichever 14-bit `FAN_ID` the
remote transmits during a pairing window:

1. Remove the battery cover on the remote — there's a "Learn Switch"
   underneath.
2. Cut power to the ceiling fan at the breaker (or wall switch) for
   one minute.
3. Restore power. You have 60 seconds to pair.
4. Press and hold the Learn Switch with a pen or paperclip until the
   light on the fan blinks twice, then release.

In principle you can do the same thing from this daemon by sending the
`pair` command (`0100100` in the protocol table) within the pairing
window, with whatever 14-bit `FAN_ID` you've put in `config.yaml` —
the fan should latch onto it and ignore the original remote. **I've
never actually tried this**; I sniffed my existing remotes instead.
If you do try it, please open an issue and let me know whether it
works.

Decoding an existing remote's ID
--

If you'd rather mirror an existing remote than assign a new ID, use an
RTL-SDR dongle to capture the remote's transmission and extract the
14-bit `FAN_ID`. Follow
[this tutorial](https://www.youtube.com/watch?v=_GCpqory3kc), or use
`utils/decode_fan_remote.py` on a recorded capture.

Sending the remote control signal
--

You can use a vanilla Raspberry Pi of any kind to transmit control
signals (I used a Pi 0), using [rpitx](https://github.com/F5OEO/rpitx),
at least in a hacky way, [without additional or customized hardware](https://www.youtube.com/watch?v=3lGU7PjJM7k).

This only serves as a remote control — we don't know what the fans are
actually doing, we just send commands blindly and hope it works, like
the remotes do.

Architecture
--

The daemon (Rust, in `src/`) runs on a Raspberry Pi and bridges both
Home Assistant and HomeKit to the physical fans:

1. **Startup** — loads persisted state, then (optionally) calls the
   HA REST API to initialise virtual fan/light entities.
2. **Command reception** — either
   - **HA**: a persistent WebSocket authenticated with a long-lived
     access token, listening for `call_service` events; or
   - **HomeKit**: a native HAP bridge (via vendored `hap-rs`) that
     iOS Home discovers via Bonjour and talks to directly.
3. **RF transmission** — when a command arrives, it shells out to
   `sendook` (from `rpitx`) to transmit the 25-bit OOK frame on the
   Pi's GPIO pin, mimicking the original remote.
4. **State updates** — after each command, state is persisted locally,
   pushed to HA via REST, and mirrored into the HomeKit characteristics.

No external broker required. Either or both integrations can be
disabled in the config.

How to use this repository
--

Cross-compile for the Pi (any ARMv6HF target, e.g. Pi Zero / Pi 1):

```
cargo zigbuild --release --target arm-unknown-linux-gnueabihf
```

Copy `config_sample.yaml` to `/etc/onlyfansd/config.yaml` on the Pi and
fill in your HA URL + token and the 14-bit `FAN_ID` for each fan
(pair it via the SET-button procedure above, or sniff an existing
remote). Deploy the binary to `/usr/local/bin/onlyfansd` and run it
under systemd.

Integrations
--

- **Home Assistant**: The daemon creates entities via the REST API
  (`/api/states/`), which means they appear in **Developer Tools →
  States** but are not backed by an integration — HA will not
  auto-add them to your dashboards. To give them persistent
  `unique_id`s so they're UI-manageable, drop `ceiling_fans.yaml`
  into your HA config directory and add
  `template: !include ceiling_fans.yaml` to `configuration.yaml`.
- **HomeKit**: set `homekit.enabled: true` in the config. The bridge
  is advertised via mDNS. Pair using the PIN you set in
  `homekit.pin` (default `03141592`). Pairing data persists in
  `homekit.persist_dir`.

Set `homeassistant.enabled: false` (or omit the whole
`homeassistant:` section) to run HomeKit-only.

`utils/` — Python reference implementation
--

The original Python implementation and a couple of small utilities live
in `utils/`. They are not needed at runtime; keep them around as a
readable reference for the RF protocol, and to help bring up new fans.
See [utils/README.md](utils/README.md) for details.

Debugging a fan that won't respond
--

Compare the signal this daemon sends out against what the actual remote
sends out. If you suspect the fan lost its pairing (e.g. after a long
power outage), re-pair it with the SET-button procedure above.
