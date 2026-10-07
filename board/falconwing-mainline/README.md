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

Same card layout and FAT config partition as `board/falconwing`
(network-config / user-data / authorized_keys); the userland scripts are
shared via `board/falconwing/rootfs-common`.

## What's in the kernel

| Hardware | How |
|---|---|
| Debug UART (PWM0/1) | `ttyAMA0`, 115200; `DEBUG_LL` + `earlyprintk` |
| Watchdog (armed for 20 s by the bootloader) | patch 0001: core keeps it fed until userspace opens it |
| microSD (SSP1) | `mxs-mmc`, root on `mmcblk0p2` (ext4) |
| RTC | `stmp3xxx-rtc` |
| USB host + internal wifi | ChipIdea + mxs PHY; power/reset lines as GPIO hogs/regulator; `rt2800usb`/`rt73usb` |
| I2C | `i2c-mxs`, QN8005 tuner at **0x2b** (7-bit) |
| LCD 320x240 | `mxsfb` + patch 0002 (`chumby,falconwing-lcd` panel-simple, RGB666) |
| Backlight | `pwm-backlight`, PWM2 @ 625 Hz |
| Touchscreen | `mxs-lradc-ts`, 4-wire |
| Knob | `rotary-encoder` on ROTARYA/B as GPIOs (REL_WHEEL) |
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
- **chumby-shell** (package/chumby-shell): panel + home windows. Tap an
  icon to switch, long-press to close, ⌂ minimizes all (shows home). The
  volume and brightness buttons open a big popup over the app area: a thick
  bar you tap to set (no knob to aim at) and - / + buttons in 10 % steps;
  it closes on the same button, a tap on empty space, or after 6 s idle.
  Turning the knob changes the volume from anywhere and shows the popup.
  `killall -USR1 chumby-shell` / `-USR2` opens the popups (debugging).
  Started with the compositor by `/etc/init.d/S95chumby-session` (log in
  /var/log/chumby-session.log).
- **libchumby** (package/libchumby): the framework every app links. LVGL
  9.6 on Wayland (wl_shm), Noto Sans via FreeType (Montserrat bitmaps only
  as fallback for LVGL's symbol glyphs), the universal dark theme (a child
  theme of LVGL's default that styles buttons/sliders from the palette),
  automatic night mode, volume/brightness helpers, and a main loop that can
  watch extra fds. See `chumby.h`; a minimal app is ~10 lines.
- **Apps** (package/chumby-apps): `chumby-clock`, `chumby-fm`. An app is a
  binary plus `/usr/share/chumby/apps/<name>.app` (`Id` = the app_id it
  passes to `chumby_app_init()`, `Name`, `Exec`, `Icon`).

**Night mode** is policy, not per-app: `/etc/chumby/policy.conf`
(`night_start = 19:00`, `night_end = 07:00`, `mode = auto|day|night`,
`day_brightness`, `night_brightness`) is re-read every 30 s by every
process. At night the palette turns black/warm-red and the shell dims the
backlight; the brightness control overrides until the next switch.

**Touch** goes through libinput; the vendor calibration is applied by
`/etc/udev/rules.d/90-chumby-touch.rules` (`LIBINPUT_CALIBRATION_MATRIX`).

No HarfBuzz: LVGL lays text out per codepoint (fine for Latin, Greek,
Cyrillic; not for complex scripts). No keyboard layouts are installed
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
- Audio driver: written against the vendor/Freescale init sequences, never
  run on this kernel.
- FM tuning matches the stock tool, but the software `scan` is new.
- Touch axis orientation/calibration on the mainline LRADC driver.
- The GUI was exercised end to end on wlroots' headless backend under
  qemu (compositor, shell, apps, switching state, night mode), but not on
  the DRM/libinput path: mxsfb page flips, libinput touch, CPU/RAM load of
  pixman compositing at 454 MHz with 64 MB.
