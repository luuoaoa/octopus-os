# The Chumby SDK: apps without buildroot

Write an app for the Chumby's UI on any Linux PC with Docker, see it in a
simulator, and install it on a device, from a USB stick or over the
network. The SDK targets one board: by default any 64-bit Raspberry Pi
running the Chumby's UI (`raspad`), or the Chumby One itself
(`falconwing_mainline`), or a board of your own. You build it once, from
this repository: [sdk-build.md](sdk-build.md). Then a step-by-step
walk-through of an app: [sdk-tutorial.md](sdk-tutorial.md). How apps work
(the UI, sound, the traps): [writing-chumby-apps.md](writing-chumby-apps.md).

## What's in it

| | |
|---|---|
| `chumby-sdk:BOARD` (Docker image, x86-64) | buildroot's SDK for the board: its cross compiler, the device's libraries and headers (libchumby with LVGL, pixman, ALSA, libcurl, ...), `pkg-config` set up for them; `chumby-app`; the examples |
| `chumby-sim:BOARD` (Docker image, the board's CPU) | the board's own root filesystem, to run its UI on a PC through QEMU |
| `sdk/chumby-app` | the one command: `new`, `build`, `pack`, `sim`, `deploy` |
| `sdk/examples/hello` | the template `chumby-app new` copies |
| `sdk/examples/onesong` | One Song: play a song online, the tutorial's app |

## Setting up

- **Docker**, and the SDK's two images for your board, built with
  `sdk/make-sdk.sh` ([sdk-build.md](sdk-build.md)).
- **Which board:** `chumby-app` uses `CHUMBY_BOARD` (the images' tag),
  `raspad` if it's unset: `export CHUMBY_BOARD=falconwing_mainline` for the
  Chumby One. A bundle runs only on the kind of board it was built for
  (`chumby-install` checks).
- **ARM emulation for the simulator:** QEMU user emulation registered with
  binfmt_misc and the `F` flag. Distribution packages (`qemu-user-static`,
  `qemu-user-binfmt`) do this; otherwise, once per boot:
  `docker run --privileged --rm tonistiigi/binfmt --install arm,arm64`.
- **A Wayland desktop** for `chumby-app sim` (GNOME, KDE Plasma and sway are
  all Wayland by default now).
- **`chumby-app` on your PATH:** copy `sdk/chumby-app` into `~/bin`.

## An app is a bundle

A bundle is a directory (`bundle/`, as the examples' Makefiles make it):

```
bundle/
  app.conf      the descriptor
  onesong       the program (ARM, linked against libchumby)
  ...           anything else it needs: pictures, sounds (read them from
                chumby_app_dir())
```

`app.conf`:

```
Id = OneSong          # = the app_id the program passes to chumby_app_init()
Name = One Song       # in the Home menu and on its tab
Short = Song          # optional: the tab's name if Name is too long
Version = 1.0
Exec = onesong        # the program, relative to the bundle
Icon = play           # a name from the shell's icon table (see writing-chumby-apps.md)
Color = #d35400       # optional: the app's hue (its tab, CHUMBY_COLOR_ACCENT)
```

`chumby-app pack` turns `bundle/` into `<Id>.chumbyapp`: a gzipped tar of
one directory named after the Id. That's the file to hand out.

### On the Chumby

- **Installed bundles live in `/var/apps/<Id>/`**, on the SD card's
  persistent partition: root's, so apps can't change themselves or each
  other. The shell watches that directory, and an app shows up in the Home
  menu as soon as it's installed. No restart is needed.
- **An app's data** goes in its own directory, `/var/lib/chumby/apps/<Id>/`
  (`chumby_app_data_dir()`, which is also its `$CHUMBY_APP_DATA`). It's kept
  across updates.
- **Apps run as the user `chumby`,** not root, like the Chumby's own apps.
  They can play sound, talk to the shell and use the network; they can't
  change the system.
- **A bundle can't take the Id of one of the system's apps** (Clock, FM
  Radio, AirPlay, Settings). It can take the Id of one of the image's
  preinstalled bundles (Internet Radio is `NetRadio`, Podcasts `Podcasts`,
  in `/usr/lib/chumby/apps`): the installed one then replaces it - an
  update.
- **Settings an app may read or set:** libchumby's `chumby_setting_get()`
  and `chumby_setting_set()` (`/var/lib/chumby/settings.conf`; `mode` is
  night mode: `auto`, `day` or `night`).

### Installing

- **From a USB stick:** put `.chumbyapp` files (or bundle directories) in a
  folder named `chumby-apps` at the top of a FAT-formatted stick, and plug it
  into the Chumby. They're installed, and the stick is unmounted again.
  Unplug it when you like.
- **Over the network:** `chumby-app deploy <chumby's address>` (SSH, as
  root).
- **On the Chumby itself:**
  - `chumby-install <file.chumbyapp | directory | ->` installs;
  - `chumby-install --list` lists what's installed;
  - `chumby-install --remove <Id> [--purge]` removes a bundle, and with
    `--purge` its data too.

## The simulator

`chumby-app sim` runs the Chumby's UI on your PC as a 960x720 window (the
320x240 screen at 3x): the real compositor, shell and apps, the very
binaries the device runs, ARM code through QEMU. Your bundle is installed
in it, so open it from the Home menu. The mouse is the finger.

- **Your app's colours, fonts, layout and night mode** look the same as on
  the device. Speed doesn't: it's an emulated ARM on a fast PC, so it can be
  faster than the Chumby's. Check performance on the real thing.
- **Sound** plays through the PC's. The simulator has no sound card: its
  ALSA writes 48 kHz 16-bit stereo into a FIFO (`sdk/sim/asound.conf`), and
  `chumby-app sim` plays that with `pw-cat`, `paplay` or `aplay`, whichever
  it finds, mixed with the rest of the desktop. It works for any app using
  ALSA's `default` or `chumby` device, which is what the stream player and
  the alarm tone use. `--no-sound` leaves it out. The volume control
  doesn't work in the simulator (there's no mixer to set); use the PC's.
- **No buttons:** the knob, snooze and power buttons aren't there. The
  shell's FIFO stands in for them:
  `docker exec chumby-sim sh -c 'echo snooze > /run/chumby/shell.cmd'`.
- **`chumby-app sim-shot FILE`** runs it headless and saves a screenshot
  with your app in front (`FILE.ppm`), for scripts and tests.

## Making the SDK

See [sdk-build.md](sdk-build.md): `sdk/make-sdk.sh [BOARD]` builds the
board's image and buildroot's SDK from source, then the two images. An SDK
goes with the image it was made from: libchumby's interface (`chumby.h`)
can change between images. Apps built against an older one keep working
as long as the functions they use stay.

## Design notes

- **Why a simulator, not an emulator:** emulating the whole Chumby (QEMU
  system mode) would need its LCD, touchscreen and codec emulated, and
  i.MX23 support QEMU doesn't have. User-mode emulation runs the same
  binaries on the PC's kernel. wlroots' Wayland backend (always built) makes
  `chumby-wm` a window on the PC's desktop, so what you see is the real
  UI.
- **Why bundles are root's and apps aren't:** an app (one user, `chumby`,
  for all of them) can't overwrite another app's program. The Chumby treats
  apps as relatively trusted: there's no per-app sandbox. A future bundle
  format could give each bundle a user of its own.
- **No systemd unit per app:** installing never adds unit files (a
  `daemon-reload` stalls the Chumby's PID 1 past its 15 s hardware
  watchdog). USB installs use one template unit, `chumby-usb-install@`.
