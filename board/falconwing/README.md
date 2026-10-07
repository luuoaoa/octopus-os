# falconwing (Chumby One)

Buildroot userland for the Chumby One ("falconwing": Freescale i.MX233 /
STMP378x, ARM926EJ-S @ 454 MHz, 64 MB RAM, boots from microSD).

Only the userland is rebuilt. The bootloader, config block and the stock
`2.6.28-chumby` kernel are reused byte-for-byte from a stock SD card
image, and so are the kernel modules and wifi firmware (they have to match
that kernel).

Unlike pifansrf, the rootfs is **not** a cpio in RAM — 64 MB doesn't
allow it. It's a regular ext3 partition, mounted rw.

## Build

```sh
# 1. Once: pull boot partition, modules and firmware out of a stock image
#    (host side, needs python3 + debugfs; the image is only read).
board/falconwing/extract-stock.sh /path/to/stock-sdcard.img

# 2. Build (inside the usual container).
bash docker-build.sh falconwing
```

Output: `output-falconwing/images/sdcard.img`. Flash with
`dd if=sdcard.img of=/dev/sdX bs=1M conv=fsync`.

## SD card layout

| part | type | contents |
|------|------|----------|
| p1 | 0x53 | stock boot stream + config block + kernels, at sector 4 (unchanged) |
| p2 | ext3 | buildroot rootfs, 256 MB, mounted rw at `/` |
| p3 | FAT16 `CHUMBY` | config files, mounted ro at `/boot` |

The stock config block's active slot is A: `krnA` with
`console=ttyAM0,115200 init=/linuxrc root=/dev/mmcblk0p2 rootfstype=ext3 ro`.
`/linuxrc` is a shell wrapper that execs busybox `/sbin/init`.

## Configuring (from any PC, on the `CHUMBY` partition)

- `network-config` — WiFi SSID + password (Pi Imager format, same parser
  as pifansrf). Or drop a full `wpa_supplicant.conf` there instead.
- `user-data` — `hostname:`, `ssh_pwauth: true|false`, `passwd:` (crypt hash
  for root).
- `authorized_keys` — optional, installed as root's SSH keys.

Applied every boot by `/usr/sbin/sdcard-config-apply`. SSH is dropbear,
`root` / `root` by default. Host keys persist on the ext3 rootfs.

## Boot sequence

`/etc/inittab` mounts proc/sys, remounts `/` rw, then `rcS` runs:

- `S00devfs` — tmpfs on `/dev`, mdev as kernel hotplug helper, `mdev -s`
  (2.6.28 has no devtmpfs). mdev also autoloads modules by MODALIAS and
  serves firmware requests from `/lib/firmware`.
- `S00watchdog` — **the bootloader arms the hardware watchdog**; load
  `stmp3xxx_wdt` and start busybox `watchdog`, or the board resets.
- `S03usb` — power-cycle USB (GPIO0 pin 26 via `devmem`, as the stock
  `reset_usb.sh`), restore clock from RTC, load `ehci_hcd`. The internal
  wifi then enumerates and its driver autoloads.
- `S30sdcard-config` — apply the config partition.
- `S40network` — wait for `wlan0`, `wpa_supplicant` (nl80211, falls back to
  wext) + `udhcpc`; also DHCP on any USB ethernet `eth*`.
- `S50dropbear`.

The serial console (ttyAM0, 115200) shows the whole boot.

## Toolchain notes

- uClibc-ng, not glibc: glibc needs a ≥ 3.2 kernel at runtime.
- Kernel headers 3.2.102: oldest buildroot offers; headers < 5.1 also keep
  uClibc-ng off time64 syscalls. Anything newer than 2.6.28 that a program
  calls returns ENOSYS — busybox (`init_module`), dropbear (`getrandom` →
  `/dev/urandom`) and wpa_supplicant all have fallbacks.
- devpts on 2.6.28 rejects `ptmxmode=`, hence the board's own `fstab`.

