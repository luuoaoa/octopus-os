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
| Squeeze switch | `gpio-keys`, keycode 130 |
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
DRAM/EMI is unaffected. Check `cat /sys/kernel/debug/clk/clk_summary` and
`/sys/devices/system/cpu/cpu0/cpufreq/` on first boot; if the boot rate
isn't 454.74 MHz the OPPs still work but won't be exact.

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

## Unverified until it runs on hardware

- Panel dot clock (vendor: 3.3 MHz ≈ 29 Hz) and LCD reset polarity.
- USB power vs reset roles of B0P26 / B0P29 (vendor comments disagree;
  end state is both high either way).
- Knob steps per detent.
- Audio driver: written against the vendor/Freescale init sequences, never
  run on this kernel.
- FM tuning matches the stock tool, but the software `scan` is new.
