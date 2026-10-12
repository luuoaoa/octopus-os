# rpi64: any 64-bit Raspberry Pi

`configs/rpi64_defconfig` builds Octopus OS for any 64-bit Raspberry Pi
- 3, 3B+, Zero 2 W, CM3, 4, 400, CM4, 5, 500, CM5 - with any screen,
touch or a mouse, and speakers. One image boots them all.

Its first testing ground was a SunFounder RasPad 3 (a Pi 4B in a tablet
case: a 10.1" 1280x800 HDMI panel, USB touch, speakers on HDMI), the way
the Chumby One was the first for Octopus OS itself. That's the setup that
has run on real hardware; the rest is built and should work, and where a
part needs configuring this says how.

```sh
bash docker-build.sh rpi64
# -> output-rpi64/images/sdcard.img: write it to a card (an imager or dd)
bin/update-rpi64.sh <device address>   # later updates, over the network
```

## Which Pi

One kernel (the Pi tree's bcm2711 config, which boots all of them) and
every model's DTB; both firmware sets on the FAT (`start.elf` with
`bootcode.bin` for the Pi 3 and Zero 2 W, `start4.elf` for the Pi 4; the
Pi 5 boots from its EEPROM). The firmware picks the DTB and maps overlays
to their Pi 5 versions. Wi-Fi (brcmfmac) and Ethernet both get DHCP. The
serial console is `serial0`: GPIO 14/15 on every model, 115200
(`dtparam=uart0_console` on the Pi 5); Bluetooth is off.

## Display: 320x240 at a whole-number scale

The UI is drawn at 320x240 (the Chumby One's screen). octopus-wm shows it
at the largest whole-number scale that fits the screen - 3 on a 1280x800
panel (960x720, with a black border), 4 on 1080p - and scales every
window nearest, so pixels stay square and sharp. Nothing in the apps
changes. A screen whose own mode is bigger than 1920x1200 (a 4K TV) is
driven at the biggest mode that isn't. Try a size headless with
`OCTOPUS_WM_SIZE=1280x800`.

The display runs through the kernel's KMS driver (`dtoverlay=vc4-kms-v3d`
in config.txt) and wlroots' DRM backend, rendered on the GPU (wlroots'
gles2 renderer on Mesa: `v3d` on a Pi 4/5, `vc4` on a Zero 2 W/3). If EGL
won't start, octopus-wm falls back to software (pixman) and logs which
renderer it got.

The UI has a brightness control only where the screen has a kernel
backlight driver; otherwise use the screen's own buttons. Night mode
darkens the UI either way.

## Touch and mouse

- **USB touch panels** (most HDMI touchscreens) are HID multitouch
  devices: `hid-multitouch` is built in and they work without setup. The
  RasPad's (an ILITEK ILI2511, USB 222a:0001) is one. libinput hands
  octopus-wm coordinates normalised over the panel, and octopus-wm maps
  them over the whole output, so a touch lands on the scaled UI where the
  finger is and a touch on the border presses nothing.
- **Other touch controllers:** the official Raspberry Pi DSI touchscreen,
  Goodix, FocalTech/EDT and ADS7846 drivers are built as modules, untested;
  add the panel's `dtoverlay=` line to `config.txt` on the card.
- Capacitive panels come calibrated. (Only the Chumby One's resistive
  screen runs the touch calibration at first start.)
- **A USB mouse** moves a cursor kept to the UI, a translucent disc on the
  display's hardware cursor plane; it hides after a touch or 5 s still.
  Clicks act like taps.

## Sound

The Pi plays through PipeWire: it has the CPU for it, and it lets small
speakers be improved on the way.

    octopus-stream, alarm tone  (ALSA "octopus" / "default", asound.conf)
      -> PipeWire (system-wide; WirePlumber)
      -> "raspad-eq" sink: bankstown bass enhancer -> parametric EQ
      -> the speakers' sink ("raspad-speakers": the first HDMI port)

- `/etc/asound.conf`: ALSA's `default` and `octopus` (what octopus-stream
  opens) are PipeWire; so is the `Master` control, the default sink's
  volume, which the shell's volume slider drives. Players mix (an alarm
  over the radio).
- `/etc/pipewire/pipewire.conf.d/30-raspad-eq.conf`: the filter chain,
  always the default sink, every stage neutral until set:
  [bankstown](https://github.com/chadmed/bankstown) (package/bankstown, an
  LV2 plugin) synthesises the harmonics of the bass small speakers can't
  reproduce; then per channel a pre-gain, a low shelf, 10 peaking
  filters, a high shelf and a low-pass. (A filter's type can't change
  while PipeWire runs it, hence fixed slots.)
- `octopus-sound apply` sets them live from Settings > Sound
  (`settings.conf`: `bass = off|low|medium|high`,
  `eq = off|warm|bright|custom`); `octopus-sound.service` applies them at
  boot. Custom is `eq.txt` on the boot FAT (`/boot`, next to `user-data`)
  in Room EQ Wizard's "Filter settings as text" export (or Equalizer
  APO's): a `Preamp: -4 dB` line, up to 10 `PK` filters, one `LS`, one
  `HS`, e.g. `Filter 1: ON PK Fc 200 Hz Gain -3 dB Q 1.5`. It's read at
  boot (`sdcard-config-apply` copies it to `/run/octopus-config/eq.txt`:
  `/boot` is root's only, for the Wi-Fi password).
- `raspad-audio-setup.service` puts the speakers' sink at full volume at
  boot, so the UI's volume is the only one that matters.

**Speakers elsewhere than HDMI:** the chain plays to whichever sink is
named `raspad-speakers`, which
`/etc/wireplumber/wireplumber.conf.d/50-raspad-speakers.conf` gives to the
first HDMI port's. For the 3.5 mm jack (not on a Pi 5; its sink is
"Headphones"), a USB sound card or an I2S amplifier board, change that
file's match to your output's node name (`wpctl status` and
`wpctl inspect` on the device show it), and for an I2S board add its
`dtoverlay=` to `config.txt`. (The names say "raspad" because that's
where the chain was first tuned.)

## What differs from the Chumby One

- No FM tuner: the FM app, `radio` and the station art lookup are left out
  (`post-build.sh`).
- No knob or snooze button. Snoozing an alarm is the Clock app's button.
- Root: a plain read-write ext4 on p2. The boot FAT (p1) holds the Pi
  firmware and the same config files as the Chumby One's config
  partition (`network-config`, `user-data`, `authorized_keys`, `eq.txt`),
  mounted read-only at `/boot`.
- Sound through PipeWire and the speaker chain (above); the Chumby One
  plays to its codec directly.
