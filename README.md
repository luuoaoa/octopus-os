# Octopus OS

A small Linux system for bedside clock radios, born as a new firmware
for the **Chumby One** (2009) and also running on a Raspberry Pi. It is an
independent project, not affiliated with Chumby Industries; "Chumby One"
here only names the hardware.

The Chumby One is a Wi-Fi bedside clock and internet radio with a
320x240 touchscreen, a volume knob and a snooze bar on top. On it,
everything is replaced except the stock bootloader: a mainline
**Linux 6.19** kernel, a buildroot userland with systemd, a Wayland
compositor, and a set of apps written for the little ARM9.

```
Freescale i.MX23   one ARM926EJ-S core, 454 MHz, no FPU
64 MB RAM          320x240 resistive touchscreen, 18-bit panel
FM tuner (QN8005)  built-in audio codec, speaker + headphones
microSD            holds everything; USB for Wi-Fi
```

## What it does

- **Clock with alarms.** Big date, alarms in a text file, snooze; night
  mode with a dim, warm palette. The brightness is remembered per mode.
- **FM radio** with RDS: station name, programme text, artist and title.
  The tuner's odd I2C protocol is driven over a bit-banged bus.
- **Internet radio and podcasts:** Icecast/Shoutcast, HLS, MP3 and AAC
  (fixed-point decoders: there's no FPU), resumable episodes, a seekable
  progress bar.
- **AirPlay speaker** (shairport-sync), with remote control of the phone:
  play/pause, next/previous.
- **Flying Toasters,** After Dark's screen saver, flying over a blurred
  snapshot of the screen.
- **Buttons:** the power button turns the screen off and on (hold 2 s to
  shut down); the snooze bar snoozes alarms.
- **Settings**, by category: night mode, the time zone, the SSH server
  (off by default; turning it on makes a random root password, shown
  there), and the speakers' bass and EQ where there are any (the RasPad).
- **Apps you can install:** bundles built with the SDK, installed from a
  USB stick or over the network. See [Writing apps](#writing-apps).

Under the hood:
- a read-only, zstd-compressed root filesystem on the card;
- over-the-air updates by rsync;
- apps run as an unprivileged user, and background apps are closed when
  memory runs short;
- a "run overlay" to try builds without reflashing.

## Getting started

You need Docker, and an image of the stock Chumby One SD card: the stock
bootloader is reused, and its boot partition comes from that image.

```sh
git clone --recurse-submodules git@github.com:luuoaoa/octopus-os.git
cd octopus-os
board/falconwing/extract-stock.sh /path/to/stock-sdcard.img   # once
bash docker-build.sh falconwing-mainline
# -> output-falconwing-mainline/images/sdcard.img
```

The build runs in a pinned Ubuntu 20.04 container: buildroot's host tools
bake in their install path, so don't run them outside it. Buildroot is a
submodule, pinned at release 2026.02.1. If you cloned without
`--recurse-submodules`, run `git submodule update --init`.

Flash `sdcard.img` to a microSD card. Before the first boot, edit the
files on its small FAT partition (any desktop mounts it):

- **`network-config`:** your Wi-Fi.

  ```yaml
  version: 2
  wifis:
    wlan0:
      dhcp4: true
      access-points:
        "MySSID":
          password: "MyPassphrase"
  ```
- **`user-data`:** the hostname, the time zone (the clock syncs over NTP),
  SSH password login, and optionally a new root password.
- **`authorized_keys`:** SSH keys for root.

They're applied at every boot, so edit and reboot to change them.

SSH is off by default. Turn it on in **Settings > Network** (it makes a
random root password and shows it), or put `ssh: true` in `user-data` to
have it on from the first boot (then log in with keys from
`authorized_keys`, or the default password `root`).

### Updating a running Chumby One

```sh
bash docker-build.sh falconwing-mainline
bin/update-chumby.sh <the Chumby's address>
```

This rsyncs only the changed blocks of the root image (typically a few
hundred KB), the early-boot files and, if it changed, the kernel slot
(read back and verified). Then it reboots. No reflashing.

## Writing apps

Apps are C programs on **liboctopus**: LVGL 9 plus Octopus's theme,
fonts, sound and system helpers.

- **[docs/sdk.md](docs/sdk.md):** the SDK. Build apps on any Linux PC with
  Docker, without buildroot, and see them in a simulator: Octopus OS's
  real compositor, shell and apps, running under QEMU as a window on your
  desktop. Package them as `.octopusapp` bundles, installed from a USB stick
  or with `octopus-app deploy`.
- **[docs/sdk-build.md](docs/sdk-build.md):** building the SDK yourself,
  with `sdk/make-sdk.sh`: for a Raspberry Pi by default, the Chumby One, or
  a board of your own.
- **[docs/sdk-tutorial.md](docs/sdk-tutorial.md):** a walk-through building
  One Song, an app that plays a song from the internet.
- **[docs/writing-octopus-apps.md](docs/writing-octopus-apps.md):** how the
  UI and sound fit together, and the traps.

## Working on the firmware

- **[board/falconwing-mainline/README.md](board/falconwing-mainline/README.md)**
  is the detailed reference: the boot chain, the read-only root, the
  kernel patches, FM, the GUI stack, and what's unverified.
- **`bin/chumby-try`** tries builds without reflashing, through the *run
  overlay*: a script the early boot runs to bind test files over the image.
  It disables itself before running, so a broken test build can't run
  twice, and the compositor frames the screen in magenta while it's
  active. It also measures CPU on the device (`perf`, and repeatable `bench`
  scenarios with the sound muted).
- **Kernel patches** are in `board/falconwing-mainline/patches/linux/`:
  - the bootloader-armed watchdog;
  - the LCD panel;
  - a new driver for the i.MX23's audio codec;
  - the CPU clock divider;
  - the touchscreen probe;
  - recovering from LCD scanout underflow;
  - the power button and power-off.

A few things about this board that cost real time to learn:
- **The bootloader arms a hardware watchdog,** and systemd feeds it every
  15 s. Anything that stalls PID 1 that long, such as
  `systemctl daemon-reload` on 64 MB of RAM, resets the board.
- **The screen must stay 32-bit:** the LCD controller scrambles 16-bit
  pixels on this 18-bit panel. Screenshots read the compositor's buffer,
  not the glass, so they won't show it.
- **Measure on the device:** QEMU doesn't model the ARM9's caches or
  timing.

## Repository

```
configs/falconwing_mainline_defconfig   the Chumby One image
board/falconwing-mainline/              its board files, kernel patches, root overlay
board/falconwing/                       the stock-kernel image (and the shared overlay)
package/
  octopus-wm/        the Wayland compositor (wlroots)
  octopus-shell/     tab bar, Home menu, alarms, buttons, screen saver policy
  octopus-apps/      the system apps: clock, FM, AirPlay, Settings, the stream
                    player, Flying Toasters
  octopus-bundled-apps/  builds apps/ as preinstalled app bundles
  liboctopus/        the app framework (LVGL 9 compiled in), the saver kit
  qn8005/           the FM tuner tool and RDS decoder
  wlroots-pixman/   wlroots without a GPU
apps/               Internet Radio, Podcasts: app bundles, built like anyone's
sdk/                octopus-app, the SDK's Docker images, examples
docs/               writing apps, the SDK, the tutorial
bin/                update-chumby.sh, chumby-try
buildroot/          submodule
```

## Other targets in this repository

- **`configs/raspad_defconfig`:** the same UI and apps on any 64-bit
  Raspberry Pi, made for a RasPad 3 (a Pi 4 tablet with a 1280x800
  screen). It renders on the GPU, with see-through blurred popups and
  saver. See [board/raspad/README.md](board/raspad/README.md).
