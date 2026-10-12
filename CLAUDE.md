# Notes for Claude

What I learned working on Octopus OS that isn't in `README.md` or the
board READMEs. Read this before starting; it'll save real time.

The project is Octopus OS; "Chumby One" (falconwing) is only the name of
one board it runs on. Don't call the software Chumby.

## Build environment

- **All buildroot work happens inside the container.** The host tools
  under `output-*/host/` hard-code their install prefix as `/work/...`
  because that's where they were built. Run them outside the container
  and they look for shared Perl/m4/etc. files under `/work/...` and
  crash. A build failure with a `/work/...` path in `@INC` or similar
  means **the build was run on the host** — rerun in the container.
- Full builds: `bash docker-build.sh` (Chumby One, the default) or
  `bash docker-build.sh rpi64` (Raspberry Pi).
- Targeted rebuilds, docker by hand:
  ```sh
  docker run --rm -v "$(pwd):/work" -w /work octopus-builder:ubuntu20.04 \
      bash -c "cd buildroot && make O=/work/output-rpi64 BR2_EXTERNAL=/work <target>"
  ```
  - `<pkg>-rebuild` — re-run one package's build step. The local packages
    (liboctopus, octopus-wm, octopus-shell, octopus-apps,
    octopus-bundled-apps) are **not** rebuilt when their sources change:
    rebuild them explicitly, liboctopus first.
  - `<pkg>-reconfigure`, `<pkg>-dirclean`, `linux-reconfigure` (kernel
    config fragment changes).
  - `make` (no target) — reassembles the root filesystem and images.
- Root overlay changes and `post-build.sh` edits are picked up by the
  next `make`. A file removed from an overlay stays in `output-*/target`
  until you delete it there.

## Backgrounding long builds

- Run builds with `run_in_background: true`, logging to a file, and emit
  the real exit status: `...; echo "EXIT=${PIPESTATUS[0]}"`. The exit code
  of a `| tee` is tee's, not the build's.
- Real failures:
  ```
  grep -E "^make.*Error [0-9]|FAILED|fatal:|hash mismatch|Temporary failure" \
       build.log | grep -v ignored | tail
  ```

## Git workflow specifics

- Commit messages: short imperative title, blank line, a body explaining
  the *why*. Do not do the co-authored by Claude shit.
- **Never push without explicit user confirmation.** History rewrites
  (force-push) need a separate explicit OK on top of that.

## Buildroot is a submodule

- Pinned at upstream `0141ca3fa5` (release 2026.02.1), from
  `https://gitlab.com/buildroot.org/buildroot.git`.
- Fresh checkouts need `git submodule update --init`.
- To bump: `cd buildroot && git fetch && git checkout <new-ref> && cd ..
  && git add buildroot && git commit`, then rebuild from scratch and
  expect kernel-fragment / package mk fallout.

## The Chumby One (falconwing)

- The bootloader arms a hardware watchdog; systemd feeds it every 15 s.
  **Never `systemctl daemon-reload`** on it: it stalls PID 1 past that.
- Test builds through the run overlay (`bin/chumby-try`), files in
  `/var/try` on the SD card, never `/tmp` (64 MB of RAM). Mute the sound
  for tests (`amixer -q sset DAC 0 mute`).
- The screen stays 32-bit: 16-bit works since kernel patch 0008, but the
  compositor's conversion costs more CPU than it saves. Screenshots read
  the compositor's buffer, so check display changes by eye; no register
  has been found that shows a split picture.
- OTA: `bin/update-chumby.sh <address>`.

## The Raspberry Pi (rpi64)

- OTA: `bin/update-rpi64.sh <address>`. Sound goes through system-wide
  PipeWire and the `raspad-eq` filter chain, set by `octopus-sound`.
- Busybox awk there has no math library (no `^`, `exp`).
