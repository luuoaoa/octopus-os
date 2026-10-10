# RasPad 3 (Raspberry Pi 4B) with the Chumby UI

`configs/raspad_defconfig` builds the Chumby One's userland (chumby-wm,
chumby-shell, the clock and internet radio apps) for a SunFounder RasPad 3:
a Raspberry Pi 4B inside a tablet case with a 10.1" 1280x800 IPS panel on
HDMI and a USB touchscreen.

```sh
docker run --rm -v "$(pwd):/work" -w /work/buildroot pifansrf-builder:ubuntu20.04 \
    bash -c "make O=/work/output-raspad BR2_EXTERNAL=/work raspad_defconfig && \
             make O=/work/output-raspad BR2_EXTERNAL=/work"
# then write output-raspad/images/sdcard.img to a card
```

## Display: 320x240 at 3x

The UI is drawn for the Chumby's 320x240 LCD. chumby-wm gives a bigger
output the largest whole-number scale at which 320x240 fits — 3 on the
RasPad (960x720), the remaining 160 px left and right and 40 px top and
bottom black — and filters every window nearest, so pixels stay square
and sharp. Nothing in the apps changes. Try a size headless with
`CHUMBY_WM_SIZE=1280x800`.

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

The RasPad's board takes sound from HDMI to its speakers (its Volume
buttons set its amplifier). `/etc/asound.conf` sends `default` and `chumby`
(what chumby-stream opens) to HDMI through a soft volume control named
`Master`, which the shell's volume slider and knob logic drive
(`chumby-softvol.service` opens it once at boot so the control exists).
For the Pi's 3.5 mm jack instead, change the slave there to
`plughw:CARD=Headphones`.

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
- Serial console on the GPIO header's mini UART (ttyS0, 115200).
