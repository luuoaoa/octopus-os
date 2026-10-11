# Tutorial: build your own Chumby SDK

The SDK isn't handed out prebuilt: you build it from this repository, for
the board you write apps for. That takes one command and, the first time,
an hour or two of the computer's time. This page walks through it, for:

- **a Raspberry Pi** (any 64-bit one running the Chumby's UI: the
  `raspad` image) - the default;
- **the Chumby One** itself (`falconwing_mainline`);
- **a board of your own**, from a defconfig.

What comes out is two Docker images, which `chumby-app` uses
([sdk.md](sdk.md)):

| Image | What's in it |
|---|---|
| `chumby-sdk:BOARD` | the board's cross toolchain (gcc, binutils), a sysroot with its libraries and headers (libchumby, LVGL, pixman, ALSA, libcurl, expat...), `pkg-config` set up for them, `chumby-app` and the examples |
| `chumby-sim:BOARD` | the board's root filesystem, to run its UI on your PC (`chumby-app sim`) |

## What you need

- **A Linux PC, x86-64**, with **Docker** (your user in the `docker`
  group). The build runs in a pinned Ubuntu 20.04 container, so your
  distribution doesn't matter.
- **About 20 GB of disk** per board (the build tree is ~12 GB; downloads
  are shared between boards in `dl/`), and the network for the downloads.
- **Time:** an hour or two for the first build, depending on the CPU;
  later builds redo only what changed.
- For the simulator: QEMU user emulation for ARM (see [sdk.md](sdk.md),
  "Setting up").

## 1. Get the source

```sh
git clone --recurse-submodules https://github.com/luuoaoa/pifansrf.git
cd pifansrf
```

Buildroot is a submodule; if you cloned without `--recurse-submodules`,
run `git submodule update --init`.

## 2. Build the SDK for a Raspberry Pi (the default)

```sh
sdk/make-sdk.sh
```

That's `sdk/make-sdk.sh raspad`. In the build container it:

1. builds the board's image (`configs/raspad_defconfig`, into
   `output-raspad/`): the kernel, the userland, the Chumby's UI - the SDK's
   libraries are exactly the image's;
2. runs buildroot's `make sdk`: the toolchain and the sysroot as one
   relocatable tarball
   (`output-raspad/images/aarch64-buildroot-linux-gnu_sdk-buildroot.tar.gz`);
3. builds `chumby-sdk:raspad` and `chumby-sim:raspad` from it and from the
   image's root filesystem (`sdk/make-images.sh`).

The `raspad` board is tuned for a Cortex-A72 (a Pi 4) with Bootlin's
aarch64 glibc toolchain; its programs run on any 64-bit Pi (3, 4, 5, Zero
2 W, the Compute Modules) with the matching image.

When it's done:

```sh
docker images | grep chumby
cp sdk/chumby-app ~/.local/bin/        # once: chumby-app on your PATH
mkdir ~/apps && cd ~/apps
chumby-app new Hello && cd Hello
chumby-app build && chumby-app sim
```

`chumby-app` uses the `raspad` images unless told otherwise. The rest -
writing an app - is [sdk-tutorial.md](sdk-tutorial.md).

## 3. Or: the SDK for the Chumby One

The Chumby's image reuses its stock bootloader, so the build needs it,
extracted once from an image of the original SD card:

```sh
board/falconwing/extract-stock.sh /path/to/stock-sdcard.img
sdk/make-sdk.sh falconwing_mainline
```

Then tell `chumby-app` which board:

```sh
export CHUMBY_BOARD=falconwing_mainline
chumby-app build
```

The Chumby is an ARMv5 (ARM926, no FPU) with a soft-float glibc
toolchain: its bundles don't run on a Pi, and a Pi's don't run on it.
`chumby-install` checks a bundle's program against the device's and
refuses the wrong kind.

## 4. Or: a board of your own

A board is a buildroot defconfig in `configs/<name>_defconfig` that has the
Chumby's UI in it. The quickest start is a copy of the one closest to
yours:

```sh
cp configs/raspad_defconfig configs/myboard_defconfig
```

What decides the SDK - and so what your apps are built for:

| Setting | In the defconfig | raspad's |
|---|---|---|
| The CPU family | `BR2_aarch64=y`, `BR2_arm=y`, ... | `BR2_aarch64=y` |
| The CPU it's tuned for | `BR2_cortex_a72=y`, `BR2_cortex_a53=y`, `BR2_arm926t=y`, ... | `BR2_cortex_a72=y` |
| Floating point (32-bit ARM) | `BR2_ARM_FPU_*`, `BR2_ARM_EABIHF=y` (hard) or `BR2_ARM_EABI=y` (soft) | VFPv4 |
| The toolchain | `BR2_TOOLCHAIN_EXTERNAL_BOOTLIN_*` (prebuilt, glibc) or buildroot's own (`BR2_TOOLCHAIN_BUILDROOT=y`) | Bootlin aarch64 glibc |

So, for instance, a Pi 3 or Zero 2 W tuned for its own core: change
`BR2_cortex_a72=y` to `BR2_cortex_a53=y`. A board that isn't a Pi needs its
own kernel, bootloader and image settings too (buildroot has defconfigs for
many: `buildroot/configs/`); keep these for the Chumby's UI:

```
BR2_INIT_SYSTEMD=y
BR2_PACKAGE_WLROOTS_PIXMAN=y          # and _GLES2=y with a GPU
BR2_PACKAGE_CHUMBY_WM=y
BR2_PACKAGE_CHUMBY_SHELL=y
BR2_PACKAGE_CHUMBY_APPS=y
BR2_PACKAGE_CHUMBY_BUNDLED_APPS=y
BR2_TARGET_ROOTFS_TAR=y               # the simulator's image
BR2_ROOTFS_USERS_TABLES="$(BR2_EXTERNAL_PIFANSRF_PATH)/board/raspad/users.table"
```

(`libchumby`, LVGL, pixman and the rest come with those. The users table
makes the apps' user, `chumby`; the RasPad's adds it to PipeWire's group.)
Then:

```sh
sdk/make-sdk.sh myboard
export CHUMBY_BOARD=myboard
```

## Keeping it up to date

An SDK goes with the image it was built from: build both from the same
checkout, and rebuild the SDK when you update the image on your devices.
After a `git pull`, run `sdk/make-sdk.sh BOARD` again: buildroot rebuilds
what changed, which is usually minutes.

To give the SDK to someone without the build:

```sh
docker save chumby-sdk:raspad chumby-sim:raspad | zstd > chumby-sdk-raspad.tar.zst
# and on their PC:
zstd -dc chumby-sdk-raspad.tar.zst | docker load
```

## When it goes wrong

- **The build stops:** the error is at the end of the output. Network
  trouble (a download that failed) is fixed by running it again. A package
  that fails to build is in `output-BOARD/build/<package>/`; rebuilding one
  is `make O=/work/output-BOARD <package>-rebuild` in the build container
  (see `CLAUDE.md`).
- **"no ... image: build it with sdk/make-sdk.sh":** `chumby-app` is looking
  for another board's images: check `CHUMBY_BOARD`.
- **`chumby-app sim` says "exec format error":** QEMU's ARM emulation isn't
  registered (after a reboot, for instance):
  `docker run --privileged --rm tonistiigi/binfmt --install arm,arm64`.
- **"built for another kind of board" from `chumby-install`:** the bundle
  was built with another board's SDK.
