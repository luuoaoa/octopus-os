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
`board/falconwing/rootfs-common`), but systemd, and a read-only root.

## Read-only root

The stock bootloader only loads the kernel slot, so there is no separate
initramfs. SD partition 2 is a small ext4 "carrier" instead:

```
/sbin/init-ram       early boot (busybox sh), init= in the DT bootargs
/bin, /lib           busybox and the libraries it needs
/rootfs.squashfs     the whole system, zstd (64 KiB blocks)
/data/var            the persistent /var
```

The kernel mounts it as root and runs `init-ram`, which loop-mounts
`rootfs.squashfs` read-only, binds `/data/var` onto `/var`, pivots (the
carrier stays at `/media/sd`) and execs systemd. The root stays
read-only; the card is only written through `/var`: alarms and the last
FM station (`/var/lib/chumby`), SSH host keys (`/etc/dropbear` ->
`/var/lib/dropbear`), timesyncd's clock, ALSA state.

What used to be written to `/etc` at boot is in RAM instead: the image's
`/etc/{hostname,localtime,timezone,shadow,default/dropbear,
wpa_supplicant/wpa_supplicant-wlan0.conf}` and `/root/.ssh/authorized_keys`
are symlinks into `/run/chumby-config`, filled every boot from the FAT
partition by `chumby-config.service` (tmpfiles seeds `shadow` from
`/etc/shadow.default` and `localtime` with UTC first). systemd mounts a
generated `/etc/machine-id`; `systemd-update-done` is masked. `/tmp` and
`/run` are tmpfs; the journal is in RAM (2 MB).

(It first ran the squashfs from a copy in RAM, like the Pi image, but
that pinned 16 MB of the 64 and starved the display of contiguous memory.)

**Updating without reflashing**: since the squashfs is only a file,
`bin/update-chumby.sh <host>` replaces it live over SSH: rootfs.squashfs
(streamed to a temp name, SHA-256 checked, renamed), the early-boot
files, the kernel slot (only if changed, read back and compared), and
optionally `customize-chumby/{network-config,user-data,authorized_keys}`
into the FAT partition; then reboots. ~3 minutes, mostly dropbear's
crypto on this CPU.

RAM cost: none pinned; pages of the squashfs decompress into the page
cache on demand like any file system.

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
 ┌──────────────────────────────────┐   chumby-wm (wlroots, pixman, no GPU)
 │ ⌂ ▌♪ FM Radio▐ 🔔     🔊 👁 10:30 │     owns DRM + input, places windows
 ├──────────────────────────────────┤   chumby-shell: the tab bar, Home menu,
 │                                  │     volume/brightness sliders, alarms
 │   the app in front (320x208)     │   apps: chumby-clock, chumby-fm,
 │                                  │     chumby-netradio, ...
 └──────────────────────────────────┘
```

**Writing an app? Read [docs/writing-chumby-apps.md](../../docs/writing-chumby-apps.md)**:
the libchumby API, sound coordination (`chumby-audio`), the ready-made
players, and the LVGL traps we hit.

- **chumby-wm** (package/chumby-wm): a small compositor on wlroots, built
  without EGL/GBM (package/wlroots-pixman). Window policy by xdg app_id
  (see the top of `chumby-wm.c`): the shell's tab bar across the top, its
  menu and slider dropping down from it, apps maximized under it, one in
  front. Apps out of sight are frozen (SIGSTOP) until brought back, unless
  they asked to stay awake. It applies the touch calibration
  (`/var/lib/chumby/touch.cal`, live) and gives the calibration tool the
  whole screen. Apps are exported via wlr-foreign-toplevel-management;
  screencopy is enabled for `chumby-screenshot > shot.ppm`.
- **chumby-shell** (package/chumby-shell): the tab bar (Home, the app in
  front, the one before it; volume, brightness, the time), the Home menu
  of installed apps, the volume/brightness sliders, screen power, the
  snooze button (toggles the screen and brings up the Clock; held 4 s:
  touch calibration) and the **alarms** (it rings them whatever is open,
  pausing whatever plays through `chumby-audio`, and resumes it when
  dismissed). Commands go to the FIFO `/run/chumby/shell.cmd`:
  `snooze|stop|ring|volume|brightness|menu|calibrate` (handy over ssh;
  `ring` tests the alarm). Started with the compositor by
  `chumby-session.service` (logs: `journalctl -u chumby-session`).
- **libchumby** (package/libchumby): the framework every app links: LVGL
  9.6 on Wayland, Rockbox's Nimbus bitmap fonts, the day/night theme,
  layout and label helpers, and a main loop that can watch extra fds.
- **Apps** (package/chumby-apps): Clock (with alarms), FM Radio (RDS,
  station art), Internet Radio (MP3/AAC/HLS via `chumby-stream`). An app
  is a binary plus `/usr/share/chumby/apps/<name>.app`.

**Night mode and screen off** are policy, not per-app:
`/etc/chumby/policy.conf` (night window, `mode = auto|day|night`,
brightness per mode, `screen_off_start/end`, `wake_seconds`,
`snooze_minutes`, `alarm_volume`), re-read every 30 s by every process.
At night the palette turns black/warm-red and the backlight dims; in the
screen-off hours it's powered down, and chumby-wm drops touches that start
on the dark screen.

**Clock**: HH:MM (one redraw a minute), date, the next alarm; alarms in
`/var/lib/chumby/alarms.conf` (`06:30 MTWTF-- on`, editable by hand).

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
