# Notes for Claude

What I learned working on pifansrf that isn't in `README.md`. Read this
before starting; it'll save real time.

## Build environment

- **All buildroot work happens inside the container.** The host's
  `output/host/` binaries (autoreconf, pkg-config, etc.) hard-code their
  install prefix as `/work/output/host` because that's where they were
  built. Run them outside the container and they look for shared
  Perl/m4/etc. files under `/work/...` and crash. If the user reports a
  build failure with a `/work/...` path in `@INC` or similar, **the
  build was run on the host instead of inside `bash docker-build.sh`** —
  rerun in the container.
- Full build: `bash docker-build.sh`. ~15–25 min on first run, much
  shorter incremental.
- For targeted rebuilds, run docker manually:
  ```sh
  docker run --rm -v "$(pwd):/work" -w /work pifansrf-builder:ubuntu20.04 \
      bash -c "cd buildroot && make O=/work/output BR2_EXTERNAL=/work <target>"
  ```
  Useful targets:
  - `<pkg>-rebuild` — force re-run of the build step for one package
    (e.g. `onlyfansd-rebuild`, `linux-rebuild`).
  - `<pkg>-reconfigure` — for autotools/meson packages whose configure
    changed.
  - `<pkg>-dirclean` — wipe the build dir entirely; next `make` rebuilds
    from scratch.
  - `make` (no target) — reassembles the rootfs cpio + boot.vfat + sdcard.img.
- Buildroot **does not auto-detect source changes** in packages with
  `_SITE_METHOD = local` (none anymore) or in pre-staged files like
  `board/.../config.txt`. Touching `board/raspberrypi0fansrf/config.txt`
  alone won't make the new content land on `boot.vfat` — you need
  `rpi-firmware-rebuild` to re-stage it. Bit me twice.
- Kernel config fragment changes need `linux-reconfigure && make`.
- Rootfs overlay changes (`board/raspberrypi0fansrf/rootfs/...`) ARE
  picked up by the next `make` because rootfs assembly always re-runs.
- `post-build.sh` runs every rootfs assembly. Edits to it take effect
  on the next `make` without a dirclean.

## Backgrounding long builds

- Always run builds with `run_in_background: true` and pipe through
  `tee` to a log:
  ```
  bash docker-build.sh 2>&1 | tee /tmp/pifansrf-build.log; echo "EXIT=${PIPESTATUS[0]}"
  ```
- **The exit code from `tee` is not the build's exit code.** Use
  `${PIPESTATUS[0]}` and emit an explicit `EXIT=N` line — otherwise
  the task notification reports exit 0 even when buildroot died with
  `make: *** Error 2`. I got fooled by this twice.
- After completion, grep for real failures with:
  ```
  grep -E "^make.*Error [0-9]|FAILED|fatal:|hash mismatch|Temporary failure" \
       /tmp/pifansrf-build.log | grep -v ignored | tail
  ```
  Plain `Error 1`/`Error 2` lines from sub-makes are usually from
  ignored doc-build failures — the `grep -v ignored` filter helps.

## Inspecting the image

- Cpio extract (ignore the `mknod` permission warning, it's harmless):
  ```sh
  rm -rf /tmp/rootfs-check && mkdir /tmp/rootfs-check && cd /tmp/rootfs-check
  zstd -d -c $REPO/output/images/rootfs.cpio.zst | cpio -idm --no-preserve-owner --quiet
  ```
- Inspect the FAT without mounting (mtools is in the host tree):
  - `output/host/bin/mdir -i output/images/boot.vfat ::/`
  - `output/host/bin/mtype -i output/images/boot.vfat ::/config.txt`
- Read ARM ELF metadata: `output/host/bin/arm-linux-readelf -d <binary>`.
- Confirm a binary contains expected strings: `strings output/target/usr/bin/onlyfansd | grep avahi-publish-service`.

## Git workflow specifics

- Commit-message convention seen in history: short imperative title,
  then a blank line, then a body explaining the *why*. Do not do the
  co-authored by Claude shit.
- **Never push without explicit user confirmation.** History rewrites
  (force-push) need a separate explicit OK on top of that.
- The `main` branch has been force-rewritten once (to drop a vendored
  buildroot tree). If you find the user's local SHAs don't match
  `origin/main`, that may be why — check `git reflog` and the
  `.git/filter-repo/commit-map` if you need to map old to new SHAs.

## Buildroot is a submodule

- Pinned at upstream `0141ca3fa5` (release 2026.02.1). Cloned from
  `https://gitlab.com/buildroot.org/buildroot.git`.
- Fresh checkouts need `git submodule update --init` (or
  `git clone --recurse-submodules`).
- To bump: `cd buildroot && git fetch && git checkout <new-ref> && cd ..
  && git add buildroot && git commit`. Then rebuild from scratch and
  expect kernel-fragment / package mk fallout from upstream churn.

## onlyfansd is upstream now

- Source lives at <https://github.com/luuoaoa/ceiling-fans-control>.
- `package/onlyfansd/onlyfansd.mk` pins a commit; the hash file
  (`onlyfansd.hash`) is the SHA256 of buildroot's **post-vendored**
  tarball (filename suffix `-cargo4`), NOT the raw github archive.
- To bump the pinned commit:
  1. Push the new state to `luuoaoa/ceiling-fans-control`.
  2. Set `ONLYFANSD_VERSION = <new-commit-sha>` in onlyfansd.mk.
  3. Run the build once. It will fail with
     `ERROR: No hash found for onlyfansd-<sha>-cargo4.tar.gz` because
     buildroot wrote the post-vendored tarball but you don't have its
     hash yet.
  4. `sha256sum dl/onlyfansd/onlyfansd-<sha>-cargo4.tar.gz` and put that
     in `onlyfansd.hash`.
  5. Rebuild — should succeed.

## `bin/update-pi.sh`

- Uses `sshpass` (`apt install sshpass`) because dropbear regenerates the
  host key every boot, defeating any persistent known_hosts approach.
- Default Pi password: `root`. Override with `PIFANSRF_SSH_PASSWORD=...`
  in the env.
- It auto-deletes `/boot/rootfs.cpio.gz` on the Pi if present —
  transition helper from the gzip era.
- The Pi reboots ~1s after the rsync completes; you'll see the SSH
  master die. mDNS comes back in ~10–15s.

## On-Pi diagnostics

- `/usr/local/bin/boot-diag.sh` runs at boot, writes `/boot/boot-diag.txt`
  with hostname, ip link, ip addr, mount, df, journal status of major
  units, and dmesg. Read it from another machine via the FAT, or
  via SSH if the Pi is reachable.
- `userspace-heartbeat.service` blinks the green LED to signal "userland
  is up." If it's not blinking by ~30s post-boot, the boot-diag.txt is
  the first place to look.
- Common boot-diag tells:
  - `wlan0: <BROADCAST,MULTICAST>` (no `UP`/`LOWER_UP`) → interface
    isn't being brought up. Almost certainly a missing `systemd-networkd`
    (it's the thing doing `ip link set wlan0 up`, not `dhcpcd`). I broke
    this once by disabling `BR2_PACKAGE_SYSTEMD_NETWORKD` thinking we
    used `dhcpcd` exclusively — we don't, both are enabled, and networkd
    is the one bringing the link up.

## Image trim approach

- Kernel-side trims live in `board/raspberrypi0fansrf/linux.config.fragment`,
  applied via `BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES` on top of
  bcmrpi defconfig. Add `# CONFIG_FOO is not set` lines.
- Userspace: prefer disabling at buildroot Kconfig level
  (`# BR2_PACKAGE_FOO is not set` in `configs/pifansrf_defconfig`) over
  `rm -f` in `post-build.sh`. Some packages have no individual Kconfig
  toggle for specific binaries (e.g. `bootctl`, `tune2fs`, `xz` CLIs);
  those go in the post-build rm list.
- Verify trims didn't break anything by inspecting cpio (paths above)
  AND by booting a real Pi and reading boot-diag.txt.
- Trim sequence so far (one commit per category) in commits 1d5b595..fb633c3.
  Mostly done — adding pipewire / shairport-sync will reclaim the headroom.

## What's been wired up so far

The current image is reasonably tuned. Key facts the user expects you
to remember without re-reading the whole history:

- Avahi owns UDP/5353 (mDNS); systemd-resolved's mDNS is disabled via a
  drop-in. onlyfansd publishes `_hap._tcp` by spawning
  `avahi-publish-service`. Future shairport-sync will use the same
  daemon — that's why we did the avahi work.
- cpio is zstd-compressed (~27 MB). Don't change that without comparing
  decompress benchmarks; gzip and xz are both worse on ARMv6.
- `/var` is capped at 512 MiB on first boot. Existing devices keep their
  larger partition until manually wiped (the unit's
  `ConditionPathExists=!/dev/mmcblk0p2` guard).
- Bluetooth, DRM, V4L2/cameras, NTFS/BTRFS/XFS/etc., netfilter, and a
  long list of unused codecs are kernel-disabled. If something starts
  needing them, edit `linux.config.fragment` and rebuild.

## Memory

There's a per-project memory directory at
`/home/wasabi/.claude/projects/-home-wasabi-develop-pifansrf/memory/`.
Read it on session start. If you learn something durable about the
user's preferences or the project's conventions, save it there per the
auto-memory rules in the system prompt.
