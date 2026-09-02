# Ultima

Minimal Linux image that boots directly into a fullscreen Qt5/QML gauge cluster, fed
live data from a car's CAN bus. Ships on BeagleY-AI (AM67A/J722S), hardware-verified
and running.

The build is a Docker-based Yocto/TI SDK build: `beagley-ai/`, `beagley-ai/NOTES.md`.
It uses BeagleBoard.org's `u-boot-bb.org` fork (not TI's), and boots via TI Falcon
mode — the R5 SPL jumping straight to the kernel, skipping A53 SPL / U-Boot-proper /
GRUB, the biggest boot-time win on this board — **without any TI falcon patch code**:
the stock R5 SPL loads a FIT carrying ATF + OP-TEE + DM + kernel + DTB
(`tifalcon.bin`, built by
`beagley-ai/meta-ultima-beagley-ai-src/recipes-bsp/ultima-falcon-fit`), with TF-A
rebuilt for the kernel/DTB addresses.

The Qt app (`ultima-app/`) is board-agnostic and has powered earlier builds of this
project on other boards; only the BeagleY-AI Yocto build remains in the repo.

**Read `beagley-ai/NOTES.md` first** for anything touching the build, boot sequence,
kernel config, or flashing — it's the reproduction/status doc for the Yocto build,
including hardware gotchas worth knowing before touching the board again. **Read
`GAUGE-CLUSTER.md`** for the Qt app's structure (source/QML/asset layout) and the CAN
bus integration (ECU, frame map, debugging) — that content is board-agnostic and
doesn't belong in the Yocto-specific notes. **Read `DASHCAM.md`** for the dash-cam
recording feature — continuous Wave5 hardware-H.264 recording of the camera streams
to an auto-mounted USB drive: its design, the on-hardware encoder bring-up
(`beagley-ai/wave5-enc/`), and milestones.

## Repo layout

- `ultima-app/` — the Qt app source. Board-agnostic; the Yocto build bind-mounts it
  read-only and builds it via its own recipe
  (`beagley-ai/meta-ultima-beagley-ai-src/recipes-ultima/ultima-app/`).
- `scripts/` — `dev-build.sh` and `dev-build-wsl.sh` are local native dev builds of
  the Qt app (macOS/WSL2), don't depend on target hardware.
- `beagley-ai/` — the whole board build: the Docker/TI-SDK build tooling
  (`Dockerfile`, `run.sh`, `build.sh`, `flash.sh`), the sole Yocto layer
  `meta-ultima-beagley-ai-src/` (app recipe, Qt5 packaging, kernel fragments,
  machine-agnostic image hardening, and the Falcon FIT recipe), and `falcon/`
  bring-up scripts. See `beagley-ai/NOTES.md` for the full story.

## Environment

- Build happens via local Docker (OrbStack) directly on this Mac —
  `beagley-ai/build.sh` / `beagley-ai/run.sh`. Build state lives in a Docker-managed
  volume (`falcon-yocto-build`), not a bind mount — see `beagley-ai/NOTES.md` for why.
  The Docker image/volume keep the legacy `falcon-yocto*` names deliberately, so the
  populated sstate cache isn't orphaned by a rename.
- The Mac can run the Qt app natively for QML/layout iteration
  (`scripts/dev-build.sh`, Qt 6 via Homebrew) — see "Local macOS Dev Build" in
  `GAUGE-CLUSTER.md`. `CanBus` simulates driving data on non-Linux instead of
  reading real CAN.
- Target hardware (BeagleY-AI + Syvecs S7+ ECU + CAN adapter) is not available in
  every session — don't assume you can flash or CAN-sniff live; ask before assuming
  access.

## Rules that are easy to get wrong

**Car / ECU:**

- **CAN1 is the powertrain bus on the ECU — never touch it.** Only CAN2 carries dash
  data; that's the only bus this project reads or writes.
- Syvecs `.SC` config files are proprietary/encrypted — don't try to parse one to
  recover CAN Tx config. Ask the user for a SCal screenshot instead.

**Yocto / board (see `beagley-ai/NOTES.md` for full context):**

- **The rootfs (`/`) is mounted read-only** — a stock, uncustomized fstab
  (`/dev/root / auto ro`). `/var/log`, `/var/lib`, `/var/cache`, `/var/spool`, and
  `/var/tmp` are symlinked/overlaid onto `/var/volatile` (tmpfs), so journald and
  coredumps land in RAM, not on the SD card. `/data` (partition 3) is the only
  persistent writable partition — that's where odometer persistence and similar app
  state belongs. For a live hand-edit (e.g. testing a systemd unit change before
  porting it to source), `mount -o remount,rw /` first — it reverts to `ro` on the
  next reboot, fstab isn't touched. Still power the board off immediately if
  `ultima-app` (or anything else) starts crash-looping rather than leave it running
  to diagnose — an earlier session saw a crash-loop produce real `I/O error`s on the
  rootfs partition.
- **This board boots from SD only (no onboard eMMC)** and its image bakes a static
  `root=PARTUUID` at build time, so `flash.sh` writes the card straight with no MBR
  disk-signature patch. The Falcon path has no interactive bootloader prompt — see
  `beagley-ai/NOTES.md` before flashing or hand-copying boot FITs.
- **`bblayers.conf` lives in the build volume and is a deliberate one-time manual
  edit** — `build.sh` refuses to auto-edit it and hard-fails if the layer isn't
  listed. See `beagley-ai/NOTES.md` if a fresh volume needs the layer wired in.

## Workflow shortcuts

- `beagley-ai/build.sh [target]` syncs the Yocto layer into the build volume and runs
  bitbake — defaults to `tisdk-base-image`. `beagley-ai/flash.sh /dev/diskN` writes the
  built image to an SD card (run it with no args to just list disks). See
  `beagley-ai/NOTES.md` for the full build/flash/verify cycle.

## Git commit conventions

- **Never add a `Co-Authored-By: Claude ...` or `Claude-Session: ...` trailer to
  commit messages in this repo.** This overrides the default commit-message
  format Claude Code normally appends. The full existing history was rewritten
  (2026-08-04) to strip these trailers specifically so Claude doesn't show up as
  a contributor — don't reintroduce them.
