# Ultima

Minimal Linux image that boots directly into a fullscreen Qt5/QML gauge cluster, fed
live data from a car's CAN bus. Ships on BeaglePlay (AM625), hardware-verified and
running.

BeaglePlay's build is Yocto/TI SDK: `beagleplay-falcon/`,
`beagleplay-falcon/NOTES.md`. It uses TI's downstream `ti-u-boot` fork rather than
mainline U-Boot because TI Falcon boot mode (R5 SPL jumping straight to the kernel,
skipping A53 SPL/U-Boot-proper/GRUB — the biggest boot-time win found on this board)
needs logic that only exists in that fork.

The project previously also shipped on Raspberry Pi 5 via a separate Buildroot build
(`br2-external/`, `SETUP-RPI5.md`). That board and its whole build system have been
removed — don't go looking for `br2-external/`, `ultima_rpi5_defconfig`, or
`SETUP-RPI5.md`; none of it exists anymore. The Qt app source that used to live at
`br2-external/package/ultima-app/` now lives at top-level `ultima-app/` (see below).

**Read `beagleplay-falcon/NOTES.md` first** for anything touching the build, boot
sequence, kernel config, or flashing — it's the reproduction/status doc for the
Yocto build, including hardware gotchas (boot-mode switch timing, a
kernel-module-autoload bug that looked like an app crash, an SD-card write-error
scare from a crash-loop) worth knowing before touching the board again. **Read
`GAUGE-CLUSTER.md`** for the Qt app's structure (source/QML/asset layout) and the
CAN bus integration (ECU, frame map, debugging) — that content is board-agnostic
and doesn't belong in the Yocto-specific notes.

## Repo layout

- `ultima-app/` — the Qt app source. Board-agnostic; the Yocto build bind-mounts
  `src/` in read-only and builds it via its own recipe
  (`beagleplay-falcon/meta-ultima-beagleplay-src/`).
- `scripts/` — `dev-build.sh` and `dev-build-wsl.sh` are local native dev builds of
  the Qt app (macOS/WSL2), don't depend on target hardware.
- `beagleplay-falcon/` — the whole BeaglePlay side: Yocto/TI-SDK (`tisdk`,
  Docker-based, runs via local Docker/OrbStack on the Mac) build producing TI Falcon
  boot mode plus `ultima-app` on top of it. `meta-ultima-beagleplay-src/` is the
  Yocto layer with the app recipe, Qt5/`linuxfb` packaging, and kernel fragments;
  `meta-falcon-beagleplay-src/` is the separate layer that wires up TI's
  falcon-boot logic (kept apart from the app layer — unrelated concerns). See
  `beagleplay-falcon/NOTES.md` for the full story.

## Environment

- Build happens via local Docker (OrbStack) directly on this Mac —
  `beagleplay-falcon/build.sh` / `run.sh`. Build state lives in a Docker-managed
  volume (`falcon-yocto-build`), not a bind mount — see `beagleplay-falcon/NOTES.md`
  for why.
- The Mac can run the Qt app natively for QML/layout iteration
  (`scripts/dev-build.sh`, Qt 6 via Homebrew) — see "Local macOS Dev Build" in
  `GAUGE-CLUSTER.md`. `CanBus` simulates driving data on non-Linux instead of
  reading real CAN.
- Target hardware (BeaglePlay + Syvecs S7+ ECU + CAN adapter) is not available in
  every session — don't assume you can flash or CAN-sniff live; ask before assuming
  access.

## Rules that are easy to get wrong

**Car / ECU:**

- **CAN1 is the powertrain bus on the ECU — never touch it.** Only CAN2 carries dash
  data; that's the only bus this project reads or writes.
- Syvecs `.SC` config files are proprietary/encrypted — don't try to parse one to
  recover CAN Tx config. Ask the user for a SCal screenshot instead.

**BeaglePlay/Yocto (see `beagleplay-falcon/NOTES.md` for full context):**

- Holding **USR** forces SD boot over eMMC — but it must be held *before* power/reset
  is applied and kept down for a couple seconds after, not tapped once boot is
  already underway. Tapping it late silently boots stale eMMC content instead, which
  looks like a real failure (hit `emergency mode` twice this way) but has nothing to
  do with anything in this repo.
- `CONFIG_DRM_TIDSS=y` gets silently downgraded to `=m` by a Kconfig dependency
  (not root-caused) — don't trust a config fragment requesting `=y` actually landed
  without checking the built `.config`. The fix in place is forcing the module to
  load via `/etc/modules-load.d/tidss.conf` rather than relying on udev coldplug,
  which real hardware showed never actually loading it.
- **The rootfs (`/`) is mounted read-only** — a stock, uncustomized fstab
  (`/dev/root / auto ro`), confirmed on both SD and eMMC. `/var/log`, `/var/lib`,
  `/var/cache`, `/var/spool`, and `/var/tmp` are all symlinked/overlaid onto
  `/var/volatile` (tmpfs), so journald and coredumps land in RAM, not on the
  physical partition. `/data` (`mmcblk0p3`) is the only persistent writable
  partition — that's where odometer persistence and similar app state belongs.
  For a live hand-edit (e.g. testing a systemd unit change before porting it to
  source), `mount -o remount,rw /` first — it reverts to `ro` on the next reboot,
  fstab isn't touched. Still power the board off immediately if `ultima-app` (or
  anything else) starts crash-looping rather than leave it running to diagnose —
  an earlier session saw a crash-loop produce real `I/O error`s on the rootfs
  partition, on a build where this may have hit real storage rather than tmpfs.
- **WiFi (onboard WL1807) is enabled as of 2026-08-19** — `wpa_supplicant`
  STA-to-"Skynet" client mode, for bench/dev SSH convenience only (not a path
  the deployed-in-car dash has any use for). It's not automatically up on a
  fresh flash: the SSID/password live in `/data/wifi-client.conf`, which is
  *not* in git or the image and has to be provisioned manually over SSH
  first (see `beagleplay-falcon/NOTES.md` "WiFi AP + captive portal
  abandoned, reverted to client mode"). An AP mode + captive-portal variant
  was also tried and fully reverted — don't assume `192.168.4.1` or a
  `hostapd`/`dnsmasq` setup is still there. Wired Ethernet remains the
  always-available path (no manual provisioning needed); serial console is
  still the fallback if neither network path is up.

## Workflow shortcuts

- `beagleplay-falcon/build.sh [target]` syncs the Yocto layer into the build volume
  and runs bitbake — defaults to `tisdk-base-image`. See `beagleplay-falcon/NOTES.md`
  for the full build/flash/verify cycle.
- eMMC (the board's default boot source, no USR held): `build-emmc-spl.sh` builds
  the eMMC-targeting R5 SPL, then `emmc-serve.sh` (Mac) + `emmc-install.sh` (board,
  booted from SD) install it. Falcon's boot device is compile-time, so **SD and
  eMMC need different `tiboot3.bin` builds** — see NOTES.md "eMMC boot".

## Git commit conventions

- **Never add a `Co-Authored-By: Claude ...` or `Claude-Session: ...` trailer to
  commit messages in this repo.** This overrides the default commit-message
  format Claude Code normally appends. The full existing history was rewritten
  (2026-08-04) to strip these trailers specifically so Claude doesn't show up as
  a contributor — don't reintroduce them.
