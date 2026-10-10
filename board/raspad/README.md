# Any 64-bit Raspberry Pi (made for the RasPad 3) with the Chumby UI

`configs/raspad_defconfig` builds the Chumby One's userland (chumby-wm,
chumby-shell, the clock, internet radio and podcast apps) for any 64-bit
Raspberry Pi - 3, 3B+, Zero 2 W, CM3, 4, 400, CM4, 5, 500, CM5 - on any
screen, with touch or a mouse. It was made for a SunFounder RasPad 3: a
Raspberry Pi 4B inside a tablet case with a 10.1" 1280x800 IPS panel on
HDMI and a USB touchscreen.

## Which Pi

One kernel (the Pi tree's bcm2711 config, which boots all of them) and
every model's DTB; both firmware sets on the FAT (`start.elf` with
`bootcode.bin` for the Pi 3 and Zero 2 W, `start4.elf` for the Pi 4; the
Pi 5 boots from its EEPROM). The firmware picks the DTB and maps overlays
to their Pi 5 versions. The serial console is `serial0`: GPIO 14/15 on
every model (`dtparam=uart0_console` on the Pi 5). Tested on a Pi 4 (the
RasPad); the rest only built.

```sh
docker run --rm -v "$(pwd):/work" -w /work/buildroot pifansrf-builder:ubuntu20.04 \
    bash -c "make O=/work/output-raspad BR2_EXTERNAL=/work raspad_defconfig && \
             make O=/work/output-raspad BR2_EXTERNAL=/work"
# then write output-raspad/images/sdcard.img to a card
```

## Display: 320x240 at a whole-number scale

The UI is drawn for the Chumby's 320x240 LCD. chumby-wm gives a bigger
output the largest whole-number scale at which 320x240 fits — 3 on the
RasPad (960x720), the remaining 160 px left and right and 40 px top and
bottom black; 4 on a 1080p screen — and filters every window nearest, so
pixels stay square and sharp. Nothing in the apps changes. A screen whose
own mode is bigger than 1920x1200 (a 4K TV) is driven at the biggest mode
that isn't: the CPU draws every pixel. Try a size headless with
`CHUMBY_WM_SIZE=1280x800`.

## Mouse

A USB mouse moves a cursor kept to the UI, an iPad-style translucent disc
(smaller while a button is down) on the display's hardware cursor plane;
it goes after a touch or 5 s without moving. Clicks act like taps.

The display runs through the kernel's KMS driver (`dtoverlay=vc4-kms-v3d`
in config.txt) and wlroots' DRM backend, rendered with pixman (no GPU).

## Touch: ILITEK ILI2511 over USB

The RasPad's touch controller is an ILITEK ILI2511 (USB 222a:0001,
"ILITEK ILITEK-TP"), a Windows-style HID multitouch device. The kernel's
`hid-multitouch` driver knows it (`MT_CLS_NSMU`); it's built in
(`linux.config.fragment`). libinput hands its absolute coordinates to
chumby-wm normalised over the whole panel, and chumby-wm maps them over the
whole output, so a touch lands on the scaled UI where the finger is and a
touch on the black border presses nothing. It's capacitive and comes
calibrated: the shell only runs the touch calibration at first start on
the Chumby's resistive screen.

## Sound

Unlike the Chumby (straight to its codec), the RasPad plays through
PipeWire: the Pi 4 has the CPU for it, and it lets the small speakers be
improved on the way. The setup comes from pifansrf's pi-radio branch.

    chumby-stream, alarm tone  (ALSA "chumby" / "default", asound.conf)
      -> PipeWire (system-wide; WirePlumber)
      -> "raspad-eq" sink: bankstown bass enhancer -> parametric EQ
      -> HDMI sink -> the RasPad's board, amplifier and speakers

- `/etc/asound.conf`: ALSA's `default` and `chumby` (what chumby-stream
  opens) are PipeWire; so is the `Master` control, the default sink's
  volume, which the shell's volume slider drives. Players mix (an alarm
  over the radio).
- `/etc/pipewire/pipewire.conf.d/30-raspad-eq.conf`: the filter chain.
  [bankstown](https://github.com/chadmed/bankstown) (package/bankstown, an
  LV2 plugin) synthesises the harmonics of the bass the speakers can't
  reproduce, at its reference strength (the Pi 0W on pi-radio had to
  run it cut down), then a by-ear EQ (+6 dB below 150 Hz, softer treble:
  -4 dB around 2.5 kHz, -6 dB above 5 kHz, a roll-off above 11 kHz)
  until the RasPad's
  speakers are measured: export Room EQ Wizard's filters as text and run
  `bin/rew-to-pipewire.py --bass export.txt`, whose output replaces the
  file's `filter.graph` block.
- `eq-toggle [on|off|toggle|status]` switches the default sink between the
  chain and the bare HDMI sink (bypass). WirePlumber remembers the choice.
- `raspad-audio-setup.service` puts the HDMI sink at full volume at boot,
  so the UI's volume is the only one that matters.

The chain's target is the first HDMI port's sink; `wpctl status` lists
the sinks if the RasPad's turns out to be another (the 3.5 mm jack is
"Headphones").

## What differs from the Chumby

- No FM tuner: the FM app, `radio` and the station art lookup are left out
  (`post-build.sh`).
- No backlight the Pi drives (the RasPad's buttons set it): the shell
  shows no brightness button.
- No knob or snooze button. Snoozing an alarm is the Clock app's button.
- Root: a plain read-write ext4 on p2. The boot FAT (p1) holds the Pi
  firmware and also the same config files as the Chumby's config
  partition (`network-config`, `user-data`, `authorized_keys`), mounted
  read-only at `/boot`.
- WiFi (brcmfmac) and Ethernet both get DHCP (udhcpc@wlan0, udhcpc@eth0).
- Serial console on the GPIO header's PL011 (ttyAMA0, 115200; pins 8, 10,
  6), Bluetooth off.
- Sound through PipeWire with the speaker chain (above); the Chumby plays
  to its codec directly.
