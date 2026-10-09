# falconwing-mainline (Chumby One on a current kernel)

The same Chumby One image as `board/falconwing`, but with a mainline Linux
kernel (6.19.x) instead of the stock 2.6.28-chumby, and a glibc userland.

Only the stock **bootloader and config block** are reused: post-image.sh
writes our `zImage` + appended `imx23-chumby-one.dtb` into the `krnA` slot
of a copy of the stock boot partition. The bootloader still does power,
DRAM, clock, USB and panel bring-up, then loads the whole slot to
0x40008000 and jumps to it.

```sh
board/falconwing/extract-stock.sh /path/to/stock-sdcard.img   # once
bash docker-build.sh falconwing-mainline
# -> output-falconwing-mainline/images/sdcard.img
```

Same FAT config partition as `board/falconwing` (network-config /
user-data / authorized_keys; the apply script is shared via
`board/falconwing/rootfs-common`), but systemd, and the system runs from
RAM.

## Running from RAM

The stock bootloader only loads the kernel slot, so there is no separate
initramfs. SD partition 2 is a small ext4 "carrier" instead:

```
/sbin/init-ram       early boot (busybox sh), init= in the DT bootargs
/bin, /lib           busybox and the libraries it needs
/rootfs.squashfs     the whole system, zstd (64 KiB blocks)
/data/var            the persistent /var
```

The kernel mounts it as root and runs `init-ram`, which copies
`rootfs.squashfs` into a tmpfs, loop-mounts it under a tmpfs overlay
(writable; changes are lost at reboot), binds `/data/var` onto `/var`,
pivots (the carrier stays at `/media/sd`) and execs systemd. After that
the card is only written through `/var`: alarms and the last FM station
(`/var/lib/chumby`), SSH host keys (`/etc/dropbear` -> `/var/lib/dropbear`),
timesyncd's clock. Everything in `/etc` that is configurable (WiFi,
hostname, timezone, password, SSH keys) is re-applied every boot from the
FAT partition by `chumby-config.service`. The journal is in RAM (2 MB).

RAM cost: the squashfs (see the post-image log line "carrier:") plus
whatever is written to the overlay; pages of it decompress on demand.

## systemd

Lean for 64 MB: no networkd, resolved, hwdb, hostnamed, timedated.

| Unit | Does |
|---|---|
| `chumby-config.service` | applies the FAT config partition (before WiFi, SSH, UI) |
| `wpa_supplicant@wlan0` + `udhcpc@wlan0` | WiFi (busybox udhcpc writes resolv.conf) |
| `systemd-timesyncd` | NTP; the kernel copies synced time into the RTC |
| `dropbear.service` | SSH |
| `chumby-session.service` | chumby-wm + chumby-shell + apps (one cgroup) |
| `serial-getty@ttyAMA0` | console |
| PID 1 | feeds the hardware watchdog (`RuntimeWatchdogSec=15s`) |

## What's in the kernel

| Hardware | How |
|---|---|
| Debug UART (PWM0/1) | `ttyAMA0`, 115200; `DEBUG_LL` + `earlyprintk` |
| Watchdog (armed for 20 s by the bootloader) | patch 0001: core keeps it fed until userspace opens it |
| microSD (SSP1) | `mxs-mmc`, `mmcblk0p2` is the carrier (see below) |
| RTC | `stmp3xxx-rtc` |
| USB host + internal wifi | ChipIdea + mxs PHY; power/reset lines as GPIO hogs/regulator; `rt2800usb`/`rt73usb` |
| I2C | `i2c-mxs`, QN8005 tuner at **0x2b** (7-bit) |
| LCD 320x240 | `mxsfb` + patch 0002 (`chumby,falconwing-lcd` panel-simple, RGB666) |
| Backlight | `pwm-backlight`, PWM2 @ 625 Hz |
| Touchscreen | `mxs-lradc-ts`, 4-wire |
| Knob | `rotary-encoder` on ROTARYA/B as GPIOs (REL_WHEEL) |
| Snooze button (top) | `gpio-keys` "snooze" on B1P30 (PWM4 pad), active low, KEY_PROG1 — pin from the vendor `chumby_bend.ko` |
| Audio | patch 0003: new i.MX23 built-in codec driver + `simple-audio-card`; external speaker amp via `simple-audio-amplifier`; headphone detect |
| CPU frequency | `cpufreq-dt`, `ondemand`: 454.74 / 227.37 / 151.58 / 113.68 MHz (patch 0004, see below) |

Not done: battery/charger/power-button (no mainline i.MX23 power driver;
the bootloader's setup is left alone), accelerometer.

### CPU frequency scaling

Down-scaling only. The bootloader sets the CPU to 454.74 MHz (PLL 480 MHz
x 18/19) with VDDD for that speed, and mainline has no i.MX23 regulator
driver to change VDDD. Patch 0004 stops CPU rate changes from rewriting
the PLL fraction (unsafe while the CPU runs from it), so cpufreq only
changes the integer DIV_CPU divider and the OPPs are the boot rate / 1-4.
Whatever the bootloader actually programmed, a request can only land at
or below the boot frequency. AHB follows the CPU (fixed HBUS divider);
DRAM/EMI is unaffected. On first boot, `cat
/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq` with the
`performance` governor shows the real boot rate (454736 expected); if it
differs the OPPs still work, just not exactly, and the top one may sit
below the boot rate. `stats/time_in_state` shows the governor at work.

The `krnA` slot is 4 MiB; the image is ~3 MB (xz, DRM/sound/USB/wifi as
modules). post-image.sh fails the build if it ever outgrows the slot.

## FM radio

`qn8005` (package/qn8005) replays the stock fmradio's register sequences
over standard i2c-dev; `radio` wraps it:

```sh
radio on 100.3            # ADC loopback: speaker + headphones
radio on 100.3 --bypass   # analog LINE1 -> headphone amp only, no CPU
radio tune 101.5
radio scan
radio off
```

**RDS**: the QN8005 decodes RDS itself (the stock tool had an `rds`
command: reg 0x00 bit 1 enables it, bit 7 of reg 0x1b flips per group,
blocks A-D in regs 0x10-0x17). While the radio is on, `qn8005 rds
/run/chumby/rds` polls it every 20 ms and keeps `ps=` (station name),
`rt=` (RadioText), `title=`/`artist=` (RadioText+) up to date; the radio
script restarts it on every tune. The chip has no block error flags, so
each segment is only taken once it arrives twice in a row. The FM app
shows the station name and "title - artist" (or the RadioText).

## GUI (Wayland)

```
 ┌────┬──────────────────────────┐   chumby-wm (wlroots, pixman, no GPU)
 │ ⌂  │                          │     owns DRM + input, places windows
 │ ♪  │   active app             │   chumby-shell
 │ 🔔 │   (maximized, one at a   │     panel: home, running apps,
 │ 🔊 │    time) or the home     │            volume, brightness
 │ 👁 │    app list behind       │     home:  installed apps
 └────┴──────────────────────────┘   apps: chumby-clock, chumby-fm, ...
```

**Alignment**: nothing is placed at a hand-tuned y. The generated fonts'
line boxes are balanced on the capitals, and layouts use libchumby's
`chumby_group()` (content-sized flex row/column, children centred on each
other) and `chumby_center_y_on/between/at()`, which centre by what is
visible: text from cap height to baseline, icons and lone punctuation by
their glyphs, groups by their children. E.g. the alarm editor's HH and MM
sit exactly midway between their + and − buttons.

Sized for the resistive touchscreen: every touch target is at least 42 px
(~9 mm) in both directions (`CHUMBY_TOUCH_MIN` in chumby.h); the panel is
64 px wide so its buttons are 56x42.

- **chumby-wm** (package/chumby-wm): ~800-line compositor on wlroots,
  built without EGL/GBM (package/wlroots-pixman: pixman renderer, DRM dumb
  buffers). Window policy by xdg app_id: `chumby-panel` pinned left (64 px),
  `chumby-home` behind everything, `chumby-popup` (the shell's transient
  controls) over the app area, any other window is an app maximized into
  the rest. Apps are exported via wlr-foreign-toplevel-management;
  screencopy is enabled for `chumby-screenshot > shot.ppm`.
- **chumby-shell** (package/chumby-shell): panel + home windows. The
  panel stays in the background: no button plates and icons at 30 %
  opacity until it's touched, then full contrast, fading back 4 s after
  the last touch (or when the popup closes). The app in front is marked
  with a dot at its left, like the macOS dock. Tap an icon to switch,
  long-press to close, ⌂ minimizes all (shows home). The
  volume and brightness buttons open a big popup over the app area: a thick
  bar you tap to set (no knob to aim at) and - / + buttons in 10 % steps;
  it closes on the same button, a tap on empty space, or after 6 s idle.
  Turning the knob changes the volume from anywhere and shows the popup.
  The **snooze button** brings up the Clock from any app, lights the
  screen for 10 s in the screen-off hours, and snoozes a ringing alarm.
  The shell also **rings the alarms** (so they go off whatever is open,
  even with the Clock closed): it brings up the Clock, keeps the screen
  on, raises the volume to `alarm_volume` if lower, stops the FM radio
  (it holds the sound card) and plays `chumby-alarm-tone`; it gives up
  after 10 min. Commands go to the FIFO `/run/chumby/shell.cmd`:
  `echo snooze|stop|ring|volume|brightness > /run/chumby/shell.cmd`
  (handy over ssh; `ring` tests the alarm).
  Started with the compositor by `chumby-session.service` (logs:
  `journalctl -u chumby-session`).
- **libchumby** (package/libchumby): the framework every app links. LVGL
  9.6 on Wayland (wl_shm); Rockbox's Nimbus bitmap fonts as the only
  text font (14 px body, 19 px headings, 35 px big values, 35 px at 2x
  for the clock face; BDFs converted to LVGL fonts at build time by
  `fonts/bdf2lvgl.py`), with LVGL's Montserrat only for the icon
  glyphs; the universal dark theme (a child
  theme of LVGL's default that styles buttons/sliders from the palette),
  automatic night mode, volume/brightness helpers, and a main loop that can
  watch extra fds. See `chumby.h`; a minimal app is ~10 lines.
- **Apps** (package/chumby-apps): `chumby-clock`, `chumby-fm`. An app is a
  binary plus `/usr/share/chumby/apps/<name>.app` (`Id` = the app_id it
  passes to `chumby_app_init()`, `Name`, `Exec`, `Icon`).
- **Clock**: HH:MM (no seconds: one redraw a minute), date, and a bar
  with the next alarm that opens the alarm list. Alarms: tap a row to
  edit, On/Off on the right, + to add (up to 8). The editor has big
  + / − for hour and minute (hold the minute buttons to step by 5) and a
  Repeat button cycling Once / Every day / Weekdays / Weekends. Stored in
  `/var/lib/chumby/alarms.conf` (`06:30 MTWTF-- on`, editable by hand; other
  day sets work, the UI just can't make them). While an alarm rings the
  Clock shows Snooze / Stop over everything.

**Night mode** is policy, not per-app: `/etc/chumby/policy.conf`
(`night_start = 19:00`, `night_end = 07:00`, `mode = auto|day|night`,
`day_brightness`, `night_brightness`) is re-read every 30 s by every
process. At night the palette turns black/warm-red and the shell dims the
backlight; the brightness control overrides until the next switch.

**Screen off**: from `screen_off_start` to `screen_off_end` (21:00-07:00)
the shell powers the backlight down completely (`bl_power`), not just to
a low brightness. The snooze button or the knob lights it for
`wake_seconds` (10), touches keep it lit while it is, and a ringing alarm
keeps it on. chumby-wm drops touches that start on the dark screen, so
nothing is pressed blind. `snooze_minutes` (9) and `alarm_volume` (60 %)
are in the same file.

**Touch** goes through libinput; the calibration (measured on hardware
from the four corners, see the rule) is applied by
`/etc/udev/rules.d/90-chumby-touch.rules` (`LIBINPUT_CALIBRATION_MATRIX`).

Script coverage: Nimbus 14 has Latin (with extended), Greek and Cyrillic;
19 has Latin-1 and Cyrillic; 35 only Latin-1 (it's for the clock and big
numbers). Missing characters fall back to Montserrat (ASCII), and there is
no shaping for complex scripts. No keyboard layouts are installed
(xkeyboard-config), so USB keyboards are ignored.

The whole stack can be tried without hardware: run the target's
`chumby-wm` under qemu-user with `WLR_BACKENDS=headless
WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1` (`QEMU_LD_PREFIX` = the target
dir), start apps against it, and grab `chumby-screenshot` PPMs.

## Unverified until it runs on hardware

- Panel dot clock (vendor: 3.3 MHz ≈ 29 Hz) and LCD reset polarity.
- USB power vs reset roles of B0P26 / B0P29 (vendor comments disagree;
  end state is both high either way).
- Knob steps per detent.
- Snooze button: pin and polarity are from the vendor driver's code; the
  internal pull-up is enabled in case the board has none.
- Alarm tone through the `default` ALSA device, and `bl_power` blanking on
  this panel (the alarm, snooze and screen logic ran headless, where there
  is no sound card or backlight).
- Audio driver: written against the vendor/Freescale init sequences, never
  run on this kernel.
- FM tuning matches the stock tool, but the software `scan` is new.
- RDS: enable bit and registers are from the stock tool, the decoder was
  only tested against synthetic groups.
- The GUI was exercised end to end on wlroots' headless backend under
  qemu (compositor, shell, apps, switching state, night mode), but not on
  the DRM/libinput path: mxsfb page flips, libinput touch, CPU/RAM load of
  pixman compositing at 454 MHz with 64 MB.
