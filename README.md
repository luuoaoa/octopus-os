# pifansrf

A minimal Raspberry Pi Zero W image whose rootfs lives **entirely in RAM**.
The SD card holds the kernel + a compressed cpio.lz4 initramfs on a small
read-only FAT, plus a 512 MiB ext4 partition for the few daemons that
genuinely need persistence. Everything else is tmpfs.

## Why this is interesting

- **Stateless by default.** Every boot is byte-for-byte identical. There's
  no `/etc` to drift, no package manager to break, no orphaned config left
  behind by a half-finished `apt upgrade`. If something feels wrong, power-
  cycle and you're back to a known-good state.
- **The SD card barely gets written.** `/boot` is mounted read-only most of
  the time and only flips to rw during an update. `/var` is 512 MiB of
  ext4 mounted with `noatime,commit=60` — it sees writes, but only from
  daemons that explicitly opted into persistence.
- **A "factory reset" is `rm /dev/mmcblk0p2`.** Delete the partition (or
  just `mkfs.ext4 /dev/mmcblk0p2` over it), reboot, and `create-var-
  partition.service` will recreate it on first boot.
- **OTA upgrade is rsync.** Push a new kernel + rootfs.cpio.lz4 to `/boot`
  over SSH, reboot — no overlay file system, no A/B partition gymnastics,
  no mid-flash bricks. The system you wrote into RAM at boot can't be
  damaged by a botched update of the SD card files it isn't currently
  reading from.
- **Boot fast.** lz4-compressed initramfs decompresses ~3× faster than
  zstd on ARMv6 (the trade is a larger file on the FAT, immaterial with
  400+ MB free), and there's no rootfs disk mount to fsck.

The original motivating daemon is `onlyfansd` — a HomeKit bridge for
Casa Vieja TR301A ceiling fans driven by `librpitx`. That's just an
example. The interesting thing is the runtime: a Pi 0W that runs a
long-lived daemon stably and is updateable over the wire, without
depending on writable rootfs semantics.

## What's in the image

- Linux 6.12.x (Raspberry Pi fork), bcmrpi defconfig + a fragment that
  drops every kernel module we don't use on this device (filesystems,
  DRM/media, netfilter, Bluetooth, exotic sound codecs, etc.). See
  `board/raspberrypi0fansrf/linux.config.fragment`.
- systemd init.
- WiFi: `brcmfmac` firmware, `wpa_supplicant`, `systemd-networkd`
  (which brings the link up and does DHCP).
- mDNS: `avahi-daemon` owns UDP/5353; `systemd-resolved` keeps doing DNS
  but its mDNS responder is disabled to avoid a `SO_REUSEPORT` collision.
- SSH: `dropbear`, off by default unless `ssh_pwauth: true` is set.
- Build tooling: `librpitx` + `onlyfansd` (HomeKit bridge for Casa
  Vieja TR301A fans, fetched from
  [`luuoaoa/ceiling-fans-control`](https://github.com/luuoaoa/ceiling-fans-control)).
  Drop the `BR2_PACKAGE_ONLYFANSD=y` line from `configs/pifansrf_defconfig`
  if you don't need it.

The final compressed image is **~27 MB** of cpio.zst plus a ~7 MB
zImage on a 512 MB FAT partition.

## Build

You need Docker. The build runs inside a pinned Ubuntu 20.04 container
because Buildroot's host-tools (`autoreconf`, `pkg-config`, etc.) bake in
their installation prefix at compile time — building on one host and
running on another silently breaks once any package needs to
reconfigure. The container pins that prefix to `/work/output/host`.

```sh
git clone --recurse-submodules git@github.com:luuoaoa/pifansrf.git
cd pifansrf
bash docker-build.sh
```

Output lands in `output/images/`:

- `sdcard.img` — flash this to an SD card with `dd`/Etcher/Pi Imager.
- `rootfs.cpio.zst` + `zImage` + `bcm2708-rpi-zero-w.dtb` — used by
  `bin/update-pi.sh` for in-place OTA updates.

If you forgot `--recurse-submodules`, run `git submodule update --init`
once. Buildroot is pinned at upstream `0141ca3fa5` (release 2026.02.1).

## First-boot configuration

The image accepts the same `/boot/network-config` and `/boot/user-data`
files Pi Imager writes. Drop them on the FAT partition (after flashing,
before first boot — most desktops auto-mount the FAT) and they're applied
once on first boot, then mostly ignored thereafter.

`/boot/network-config`:

```
country: US
wifi:
  access-points:
    - ssid: 'arendelle-iot'
      password: '<32-byte-hex-PSK>'
```

The PSK must be the pre-computed hex output of `wpa_passphrase` — the
parser doesn't run wpa_passphrase itself.

`/boot/user-data`:

```
hostname: my-pi
timezone: America/Los_Angeles
ssh_pwauth: true        # enable dropbear; default password 'root'
```

`pi-imager-apply.service` reads these on every boot but only acts the
first time it sees a changed value. To re-apply (e.g. you changed the
WiFi network), just edit the file on the FAT and reboot.

## SSH

`dropbear` only starts if `ssh_pwauth: true` is set in `user-data`.
Default user is `root`, password `root`. Change it with the
`BR2_TARGET_GENERIC_ROOT_PASSWD` line in `configs/pifansrf_defconfig`
if you care.

The dropbear host key is generated once on first boot and persists on
the `/var` partition (`/etc/dropbear` is a symlink to
`/var/lib/dropbear`), so subsequent reboots reuse the same key and
your SSH client won't warn about it. Reflashing the image preserves
`/var` (the FAT-only `sdcard.img` doesn't touch p2), so the key
survives that too — the only thing that resets it is wiping
`/dev/mmcblk0p2` or running on a brand-new SD card. `bin/update-pi.sh`
still passes `StrictHostKeyChecking=no` because the very first OTA
after a fresh flash sees a key it's never seen.

The image advertises itself over mDNS as `<hostname>.local`.

## OTA updates

`bin/update-pi.sh` rsyncs a fresh kernel + initramfs + DTB onto the
running Pi's `/boot` and reboots:

```sh
sudo apt install sshpass        # one-time, host side
bash docker-build.sh             # rebuild with your changes
bin/update-pi.sh my-pi.local     # push and reboot
```

What it does, in order:

1. Open one SSH master connection (sshpass-authed) so subsequent
   rsync invocations don't re-prompt.
2. Remount `/boot` rw on the Pi.
3. rsync `zImage`, `bcm2708-rpi-zero-w.dtb`, `rootfs.cpio.zst`, and
   the rpi-firmware contents.
4. Push any of `customize/{network-config,user-data,onlyfansd.yaml}`
   that exist locally.
5. Drop a stale `/boot/rootfs.cpio.gz` if there is one (transition
   helper from the gzip era).
6. `sync; remount ro; reboot`.

The Pi briefly drops the SSH session during the reboot. After ~10–15
seconds, mDNS is back and you can SSH in again.

## Persistent state (`/var`)

`/dev/mmcblk0p2` is created on first boot by `create-var-partition.service`
using `busybox fdisk` — 512 MiB starting at the next 1 MiB boundary
after the FAT partition. systemd-makefs (via the `x-systemd.makefs`
fstab option) formats it ext4 the first time it's mounted.

Because the unit has `ConditionPathExists=!/dev/mmcblk0p2`, an existing
`/var` is never overwritten by a reflash. Conversely, deleting `p2`
(via `fdisk` or by repartitioning) gives you a clean slate on the next
boot.

The only state that *belongs* on `/var`:

- `systemd-journald` rotated logs (`/var/log/journal/`).
- Daemons that explicitly opt in via `StateDirectory=` in their unit.
  `onlyfansd.service` uses `StateDirectory=onlyfansd`, which yields a
  reboot-surviving `/var/lib/onlyfansd/` for HomeKit pairing data.

Anything that writes to `/etc` or `/usr` is writing to tmpfs and will
vanish on reboot. That's a feature.

## Build identification

`/etc/motd` is stamped at build time with the git rev and timestamp:

```
  pifansrf — HomeKit/HA bridge (Casa Vieja TR301A via rpitx)
  build:  bbf8473
  date:   2026-04-27T00:50:00Z
```

A `-dirty` suffix means the working tree had uncommitted changes when
the image was built. Useful for telling whether you actually flashed
the build you think you flashed.

## Layout

```
/dev/mmcblk0p1  /boot   vfat ro,nofail        ← kernel + initramfs + config files
/dev/mmcblk0p2  /var    ext4 noatime,commit=60 ← persistent state, 512 MiB
none            /       tmpfs (rootfs cpio)    ← unpacked from rootfs.cpio.zst
```

Repository:

```
configs/pifansrf_defconfig       — buildroot config
board/raspberrypi0fansrf/
  config.txt, cmdline.txt        — RPi firmware boot config
  linux.config.fragment          — kernel-trim overrides on top of bcmrpi
  rootfs/                        — files copied verbatim into the image
  post-build.sh                  — runs against TARGET_DIR pre-fakeroot
  post-fakeroot.sh               — runs after preset-all (service enables)
  post-image.sh                  — assembles boot.vfat + sdcard.img
package/                         — buildroot package recipes (onlyfansd, librpitx, rpitx)
buildroot/                       — submodule, pinned at 2026.02.1
bin/update-pi.sh                 — rsync-over-SSH OTA helper
customize/                       — local-only overrides pushed by update-pi.sh
```

## Adding your own daemon

Drop a buildroot package recipe under `package/<your-daemon>/` and a
systemd unit. Pull the source from a github tarball via
`$(call github,user,repo,commit-hash)` and pin a hash in
`<your-daemon>.hash` — the cpio is rebuilt deterministically from
the pinned source, so you can trust the build identification.

If your daemon needs persistent state, give it a `StateDirectory=foo`
in the unit; everything else lives in tmpfs and gets a fresh slate
each boot. That's the contract.
