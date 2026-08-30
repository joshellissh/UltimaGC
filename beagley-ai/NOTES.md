# Ultima on BeagleY-AI (AM67A / J722S) — Build & Bring-up Notes

Reproduction/status doc for the BeagleY-AI Yocto build: how the image is built,
how the app and boot chain are wired, and the hardware bring-up log. The build
tooling lives in `beagley-ai/` (`Dockerfile`, `run.sh`, `build.sh`, `flash.sh`),
and the sole Yocto layer is `beagley-ai/meta-ultima-beagley-ai-src/`.

## Yocto build / app port

The board is up and running — display, touchscreen, and Falcon boot all working
as of 2026-08-30 (see the bring-up sections below). The sections here are the
build-log detail, roughly in the order they were worked, so some early
"Status / next step" notes are superseded by later sections.

The reason for this board: the AM67A's Wave5 hardware video encoder, which
unblocks 4-camera dashcam recording.

### The machine already exists — no new Yocto layer needed

`meta-ti/meta-beagle` (already vendored into the `falcon-yocto-build` docker
volume, already in `bblayers.conf`) ships `beagley-ai.conf` /
`beagley-ai-k3r5.conf` (`require j722s.inc` + `beagle-bsp.inc`), so no
from-scratch machine layer was needed.

Two things make it a genuinely different build target, not just a different
`MACHINE=` value:

- **It's locked to the BeagleBoard.org kernel/u-boot, not TI-staging.**
  `beagle-bsp.inc` only defines `bb_org-6_12` (default) / `bb_org-6_6` BSP
  variants: kernel `linux-bb.org` 6.12, u-boot `u-boot-bb.org` 2025.x. There is
  no `ti-*` BSP variant for beagle machines, so this board builds a different
  kernel recipe (`linux-bb.org`) entirely.
- **The R5/TIFS boot chain is HS-FS (High-Security Field-Securable), not GP.**
  Confirmed by the produced artifact name:
  `tiboot3-j722s-hs-fs-evm-beagley-ai-k3r5-2025.10+git-r0.bin` (also set
  explicitly: `beagley-ai-k3r5.conf` has `SYSFW_SUFFIX = "hs-fs"`). This built
  and produced a working artifact set with zero extra key-provisioning on our
  part — meta-beagle ships default/dev signing for this. It directly affects
  the Falcon work (Falcon rebuilds the R5 SPL, which goes through this signed
  chain); Falcon went through several rounds before working — see "Falcon on
  BeagleY-AI: working" below for how it was eventually wired up against
  `u-boot-bb.org`.

### Two build-blocking gaps found and fixed (both real, not speculative)

1. **`tisdk-uenv` recipe gap.** `meta-arago-distro`'s `tisdk-uenv.bb` (pulled
   into every arago image unconditionally via `arago.conf`, not machine-scoped)
   ships a `uEnv-sk.txt` template only for TI's own EVM machine name
   (`j722s-evm`), never extended to BeagleBoard.org's community machine name
   (`beagley-ai`). Same class of gap as the original Falcon wiring (a TI SDK
   recipe wired to EVM names only) — see "What was actually wrong upstream"
   above. Fixed with `beagley-ai/meta-ultima-beagley-ai-src/recipes-tisdk/tisdk-uenv/`:
   a `tisdk-uenv.bbappend` + `tisdk-uenv/beagley-ai/uEnv-sk.txt`.
   - **Real gotcha hit along the way**: an empty/comment-only `.bbappend` does
     **not** get your layer's directory added to `FILESPATH` — `FILESEXTRAPATHS`
     is not auto-extended just because a `.bbappend` exists for a recipe.
     `${THISDIR}` in *another* layer's existing bbappend (meta-ti-foundational's,
     in this case) is immediate-expanded (`:=`) to *that* layer's own directory
     at parse time and never picks up other layers. Our bbappend has to add its
     own `FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"` explicitly.
   - **This same gotcha resurfaced, and mattered more, when the override was
     briefly in a separate layer**: `ultima-app.service`'s
     beagley-ai override originally lived in the *same* layer as
     `ultima-app.bb` (`recipes-ultima/ultima-app/files/beagley-ai/`), where it
     needed **no** extra `FILESEXTRAPATHS` line — `FILESPATH`'s default search
     already includes `${FILE_DIRNAME}/files` from a recipe's *own* directory,
     auto-suffixed per active override, no bbappend content required beyond
     the recipe itself. Once split into the separate
     `beagley-ai/meta-ultima-beagley-ai-src` layer, that stopped being true —
     it's now a genuinely cross-layer override (a different layer's
     `.bbappend` contributing a file for a recipe defined elsewhere), so it
     needs the exact same explicit `FILESEXTRAPATHS:prepend :=
     "${THISDIR}/${PN}:"` treatment as the tisdk-uenv fix. See
     `recipes-ultima/ultima-app/ultima-app.bbappend` in the new layer.
2. **A leaked `local.conf` line.** The `build-beagley-ai` build directory was
   seeded from a working `local.conf` (same `DISTRO`/`IMAGE_INSTALL` baseline,
   different `MACHINE`), which brought along `DISTROOVERRIDES:append =
   ":ti-falcon"`. That line unconditionally requires `u-boot-ti-staging-falcon`
   (`ti-falcon.inc`'s `IMAGE_INSTALL:append`, itself unscoped) regardless of
   machine — broke the build with "Nothing RPROVIDES u-boot-ti-staging-falcon",
   since beagley-ai has neither Falcon wiring through that recipe nor a
   `u-boot-ti-staging` recipe at all. Removed from the beagley-ai build dir's
   `local.conf`.

Also hit and fixed: forgot to bind-mount the real `../ultima-app` host source at
`/home/builder/yocto/ultima-app-src` in the ad-hoc `docker run` invocations used
for this bring-up (unlike `build.sh`, which always does). A prior session's
`build.sh` run had left an empty directory *placeholder* at that exact path
inside the `falcon-yocto-build` volume (Docker auto-creates bind-mount target
directories, and they persist in the underlying volume after the container
exits even though the bind-mounted content doesn't) — so `ultima-app.bb`'s
`do_unpack` silently "succeeded" copying nothing, and the failure only surfaced
one task later at `do_configure` ("no `.pro` file found"). Fixed by adding the
same `-v "$ULTIMA_APP_SRC:/home/builder/yocto/ultima-app-src:ro"` mount
`build.sh` uses.

### Build setup — automated in `build.sh`

`build.sh` builds `tisdk-base-image` in `tisdk/build-beagley-ai/` (a BitBake
build dir seeded from a working `local.conf`/`bblayers.conf` with `MACHINE`
flipped to `beagley-ai` and the `ti-falcon` `DISTROOVERRIDES` line removed).
`SSTATE_DIR` points at `tisdk/build/sstate-cache` to reuse architecture-common
sstate (aarch64 toolchain, common non-machine-specific recipes) — a large
fraction of the ~8900-task build comes from there. `DL_DIR`
(`${TOPDIR}/../downloads`) is shared automatically since the build dirs are
siblings under `tisdk/`.

On a cold volume `build.sh` seeds `tisdk/build-beagley-ai/conf/` from
`tisdk/build/conf/`, flips `MACHINE`, comments out the `ti-falcon`
`DISTROOVERRIDES` line, and points `SSTATE_DIR` at the shared cache — idempotent
(a no-op once `conf/local.conf` exists). The layer is synced into
`tisdk/sources/` on every run. **Deliberately NOT automated**: adding the layer
to a build dir's `BBLAYERS` — that's a one-time, human-reviewed edit to a shared
config, not mechanical setup; a fresh volume gets a clear "add it manually once"
error naming the exact file, and `build.sh` verifies the line is present before
invoking bitbake.

The sync step stages the layer through `rsync -a --exclude='.smbdelete*'` to a
`mktemp -d` scratch dir (cleaned via `trap ... EXIT`) before bind-mounting — on
an SMB-mounted checkout a stray `.smbdeleteAAA*` ghost file (a rename-then-unlink
artifact from macOS smbfs) would otherwise break the container's `cp -a`
mid-traversal.

Verified: `./build.sh tisdk-base-image` run for real against the live volume —
sync + bootstrap succeeded, and the full image build completed clean (8900+
tasks, all succeeded, mostly served from warm sstate).

### What's in the layer

- `IMAGE_INSTALL`: `ultima-app ultima-splash can-utils mmc-utils
  ultima-data-mount volatile-binds` + the GPU smoke-test set
  (`ti-img-rogue-driver ti-img-rogue-umlibs kmscube mesa-demos`). **Not**
  installed: `ultima-hwclock-load` (hardcodes `/dev/rtc0` for a BQ32002 this
  board doesn't have — this board has a **different, but real, onboard RTC**:
  official BeagleBoard docs confirm a populated **DS1340** chip with a 2-pin
  JST SH connector for a coin-cell backup battery — see
  <https://docs.beagleboard.org/boards/beagley/ai/demos/using-rtc.html>. Not
  "no RTC" — just needs its own hwclock-load variant once RTC bring-up is
  scheduled: the current script hardcodes the wrong chip, not a missing one.
  Don't confuse this with the *separate*, genuinely optional
  `k3-am67a-beagley-ai-i2c1-rtc-rv3028.dtbo` overlay also in this board's
  `KERNEL_DEVICETREE` list — that's an add-on RTC module overlay (different
  chip, RV-3028, presumably a pluggable I2C1 module), unrelated to the built-in
  DS1340), WiFi (`wpa-supplicant`/`wl18xx-firmware`, hardware this board doesn't
  have), `mycam004m` (camera port is a later milestone).
- `WKS_FILE:beagley-ai = "ultima-beagley-ai.wks.in"` — three partitions (boot
  vfat, ext4 root, ext4 `/data`); see the file's own header for the Falcon
  boot-partition details.
- `hostname:beagley-ai = "ultimagc-beagley"` — an explicit hostname so the board
  is unambiguous on the bench network.
- `PACKAGECONFIG:append:beagley-ai = " linuxfb"` on qtbase — meta-ti's
  `PREFERRED_PROVIDER_virtual/gpudriver` wiring for this machine (`j722s.inc` +
  `beagle-bsp.inc`) already makes eglfs the default `PACKAGECONFIG_GL`; linuxfb
  added as a same-image fallback, selected via env var not rebuild.
- **`ultima-app.service`: linuxfb-first for this board** (not eglfs). New file
  at `recipes-ultima/ultima-app/ultima-app/beagley-ai/ultima-app.service`, plus
  a sibling `recipes-ultima/ultima-app/ultima-app.bbappend` with an explicit
  `FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"` so the per-override file is
  found. Deliberate: the BXS-4-64 GPU driver stack was completely unproven on
  this board. `ti-img-rogue-driver`/`ti-img-rogue-umlibs` build and package
  cleanly against the bb.org 6.12 kernel (`do_rootfs` resolved every RDEPENDS),
  so the build-time risk didn't materialize; what took first-boot verification
  was *runtime* — whether the driver initializes against real BXS-4-64 silicon
  and Qt's `eglfs_kms` finds `/dev/dri/card0`. Once eglfs is confirmed, flip
  both `Environment=` lines back to `QT_QPA_PLATFORM=eglfs` +
  `QT_QPA_EGLFS_INTEGRATION=eglfs_kms` (both together — qtbase is built
  `-opengl es2`, so linuxfb alone leaves Quick trying the GL scenegraph with no
  context).

### Verification (build-only, no board yet)

- `bitbake tisdk-base-image` for `MACHINE=beagley-ai`: all tasks succeeded
  (8918 attempted, 1 harmless QA warning — a qtdeclarative-dev header
  referencing `TMPDIR`, pre-existing upstream noise unrelated to this port).
- Package manifest confirmed: `ultima-app`, `ultima-splash`,
  `ultima-data-mount`, `can-utils`, `mmc-utils`, `volatile-binds`,
  `ti-img-rogue-driver`, `ti-img-rogue-umlibs`, `kmscube`, `mesa-demos` all
  present for `beagley_ai`/`aarch64`.
- `.wic` partition table confirmed (decompressed + `parted print`): p1 FAT16
  134MB (boot, active), p2 ext4 1400MB (root), p3 ext4 16.8MB (`/data`) — the
  intended 3-partition layout, not the vendor-default 2-partition one, so the
  custom WKS took effect correctly.
- Full boot artifact set present in `deploy-ti/images/beagley-ai/`: R5 SPL
  (`tiboot3-j722s-hs-fs-evm...bin`), `tispl.bin`, `u-boot.img`/`u-boot-spl.bin`,
  kernel `Image`/FIT image, `k3-am67a-beagley-ai.dtb` + the full set of
  meta-beagle's overlays (i2c/PWM/PPS/mikroBUS/etc.), GRUB EFI
  (`grub-efi-bootaa64.efi`).

### Flashed to SD (2026-08-29)

Pulled `tisdk-base-image-beagley-ai.rootfs.wic.xz` (+ `.wic.bmap`) out of the
volume with `docker cp` (see the deploy-dir note at the end of `build.sh`) into
`beagley-ai/deploy-beagley-ai/` (gitignored), then flashed with `flash.sh` to
the Mac's built-in SDXC reader (`/dev/disk4` this session — showed as "internal,
physical" per that script's own header comment about the reader, confirmed
removable via `diskutil info`).

**Found before flashing, and it mattered**: this board's image bakes a static
`root=PARTUUID` at build time and must NOT have its MBR disk signature patched
(an earlier flasher did that to keep an SD card's PARTUUID distinct from an
eMMC's — irrelevant here, and it would break boot; `flash.sh` now writes the
card straight, no patch). Checked by extracting p1 from the built `.wic` (`parted`
for offsets, `mtools`/`mcopy` to read the FAT partition without needing a loop
device — Docker Desktop/OrbStack on this Mac refused `losetup` even
`--privileged`, "Permission denied"/"cannot find an unused loop device").
`EFI/BOOT/grub.cfg` bakes in a **static** `root=PARTUUID=076c4a2a-02` at wic
build time:
```
menuentry 'boot'{
linux /Image root=PARTUUID=076c4a2a-02 rootwait rootfstype=ext4  ro
}
```
Since that PARTUUID is static (nothing re-derives it from the live partition
table at boot), patching the flashed card's MBR signature would desync it from
the baked-in value and the kernel would fail to find root. Moot anyway: this
board has no onboard eMMC (see "Storage" below), so the SD-vs-eMMC PARTUUID
collision such a patch exists to prevent doesn't apply here in the first place.

Not yet done: actually booting it. Next real step is serial console + power-on
verification, per this project's usual first-boot practice (see the top-level
hardware caveats in `CLAUDE.md` re: USR-button SD-boot timing).

### First power-on (2026-08-29): boot chain works, no /dev/fb0 → ultima-app crash-loops

Serial via the JST debug header + Raspberry Pi Debug Probe (`/dev/cu.usbmodem102`
on the Mac, not `/dev/cu.usbserial-*` as a CP2102-style adapter would be — a
different USB class entirely, shows up as a modem device not a serial adapter).
Full capture:
`boot-logs/beagley-ai-first-boot-20260829T180227.txt` (gitignored). Capture
contains a handful of stray NUL bytes that make `file`/plain `grep` treat it as
binary — use `grep -a`.

**The hard, genuinely-unverified parts all worked first try:**
- TIFS/HS-FS: `Authentication passed` x5, straight into `Starting ATF on
  ARM64 core...` — the HS-FS signed chain flagged as new territory in the
  Falcon feasibility check above is fine for a plain (non-Falcon) boot.
- The custom 3-partition `.wks` layout worked exactly as built: SD card
  enumerated `mmcblk1`, `mmcblk1p2` (root) and `mmcblk1p3` (`/data`) both
  mounted clean. The wks header's own "not yet verified that the ROM finds
  tiboot3.bin in p1" caveat is resolved — it does.
- Wave5 hardware encoder — the actual reason for this whole port — probed
  successfully: `vdec 30210000.video-codec: Added wave5 driver with caps:
  'ENCODE' 'DECODE'`, Product Code `0x521c`, firmware rev `363254`.

**What's broken: no display driver ever creates `/dev/fb0`.** At kernel t=5.7s,
`ultima-splash.service` ("draws once to /dev/fb0, then exits") fails outright.
`ultima-app` (linuxfb-first on this board, see above) starts at t=7.3s, aborts
immediately (SIGABRT) for the same reason, and systemd restarts it every ~5s —
27 cycles caught in the log before power was cut (per `CLAUDE.md`'s
crash-loop rule — this project has one prior storage I/O-error scare from leaving
a crash-loop running, not worth re-testing here even though this board's `/`
is presumably similarly read-only). Log has zero `drm`/`dss`/`tidss`/`fb`
lines beyond the generic `drm` core module loading via
`systemd-modules-load` — the actual display driver never probes.

**This is a display-driver-not-probing bug — the DSS driver silently never
loads.** (A previous board hit the same class of problem: `CONFIG_DRM_TIDSS`
silently downgrading `=y`→`=m`, and even as a module never auto-loading via
udev coldplug, needing an explicit forced-load.) Checked
`beagley-ai/meta-ultima-beagley-ai-src/`: it sets `QT_QPA_PLATFORM=linuxfb`
and appends the `linuxfb` `PACKAGECONFIG` to qtbase, but has **no** kernel
config fragment and no forced module-load for whatever this board's DSS
driver is — that machinery was never ported when the layer was created.
AM67A/J722S almost certainly still uses `tidss` (same TI DSS IP family across
K3 SoCs) but this hasn't been confirmed against bb.org's actual kernel config/
source tree yet — that's the next concrete step, not done this session:
1. Confirm the driver name/Kconfig symbol in the built `linux-bb.org` 6.12
   source tree (grep for `tidss` or `am67`/`j722s` display compatible
   strings under `drivers/gpu/drm/`).
2. Check whether it's `=n`, `=m`-and-uncoldplugged, or built but failing to
   bind (a device-tree node issue would be a different, worse problem than a
   mere module-loading gap).
3. Apply the same class of fix (built-in, or forced `/etc/modules-load.d/`
   entry + RDEPENDS) in the layer.

Not yet re-tested after a fix — board was powered off promptly once the
crash-loop was identified, per this project's standing rule for that
situation.

### Crash-loop fixed offline; real root cause found; a fix attempt hung the board (2026-08-29)

**Stopped the crash loop without a rebuild.** Since `/` is read-only anyway and
this project's rootfs-editing pattern is already established, masked both
units via a kernel cmdline edit on the boot partition's `EFI/BOOT/grub.cfg`
(FAT, natively mountable on macOS — no ext4/loop-device tooling needed):
appended `systemd.mask=ultima-splash.service systemd.mask=ultima-app.service`
to the `linux` line. Confirmed on reboot: unit both masked, zero crash-loop
audit lines, clean boot to a login prompt, SSH reachable over Ethernet at
`ultimagc-beagley.local` (mDNS) / DHCP-assigned IP — first live shell access
to this board.

**Root cause of no `/dev/fb0`, confirmed directly against both DTBs, not
guessed:** this board's GRUB (`EFI/BOOT/grub.cfg`) boots `/Image` with no
`devicetree` directive, so per the ARM64 Linux EFI stub's own behavior
("Using DTB from configuration table") it inherits whatever FDT U-Boot's own
EFI implementation already had registered — **U-Boot's own minimal internal
control DTB** (`u-boot-beagley-ai.dtb`, ~71KB), not the full kernel-target
`k3-am67a-beagley-ai.dtb` (~97KB) our wks build produces and puts on the FAT
partition. Verified by pulling the live tree straight off the running kernel
(`/sys/firmware/fdt` over SSH) and `dtc`-decompiling it against both
candidates: it matches `u-boot-beagley-ai.dtb` almost exactly (25 cosmetic
line diff, all phandle renumbering) and is missing huge chunks present in the
kernel-target dtb (HAT pinctrl entries, `mcu_sram1`, etc.). Smoking gun:
`u-boot-beagley-ai.dtb`'s `i2c@20010000` (`main_i2c1`, the bus carrying the
`it66122` HDMI bridge per the board's own dts) is `status = "disabled"` and
doesn't even have the `it66122` bridge node at all — U-Boot's own DTB simply
doesn't need HDMI for its own operation. The kernel-target dtb has
`main_i2c1` `status = "okay"` with the bridge node present, matching the
board's schematic-derived dts we read earlier — that dtb is correct, it's
just never the one actually handed to Linux. This explains the earlier
"only one I2C bus registers, zero dss/tidss/it66121/hdmi driver lines" boot
symptom precisely: `main_i2c1` (and `i2c2`/`i2c3`) are disabled in the DTB
actually in use, so the bridge chip's I2C bus never exists for its driver to
attach to, and tidss's component-match with it66122 never completes.

**The "obvious" fix — point GRUB at the right file — hangs the board.** Added
`devicetree /k3-am67a-beagley-ai.dtb` to `grub.cfg` (dtb copied onto the FAT
partition first) and rebooted. Result: `EFI stub: Using DTB from
configuration table` → `EFI stub: Exiting boot services...` → **complete
silence, raw NUL bytes on serial, board unreachable over the network** — a
full hang, no crash-loop audit noise, nothing. This is a genuinely different,
worse failure mode than the crash loop (silent + no network + no serial text
at all), recovered only by a full power cycle and pulling the grub.cfg change
back out (same offline FAT-partition edit, done live over the still-mounted
SD card via SSH, then again via the Mac reader once the board hung and SSH
dropped).

**Root cause of the hang is not nailed down — flagging candidate causes,
not a confirmed diagnosis:**
- Most likely: a TI SYSFW/TIFS resource-permission mismatch. K3 SoCs
  statically partition peripheral/clock/power-domain resources (TISCI board
  config) around what's negotiated during early boot; U-Boot's own DM
  devicetree is what U-Boot itself negotiated against. Swapping in a much
  larger devicetree at the Linux/EFI-stub layer — requesting many more
  peripherals than U-Boot's own resource negotiation covered — under this
  board's HS-FS (stricter than GP) secure boot chain is a plausible way to
  get an early, silent hang with zero printk, before earlycon.
- Also plausible and not ruled out: the kernel-target dtb's UART node
  (clock parent / pinmux for the earlycon path) differs subtly from
  whatever U-Boot's minimal dtb had — since **every prior successful boot
  ran on U-Boot's dtb's UART setup**, swapping to the other tree's UART
  config for the same physical console could break earlycon itself before
  any message could print, independent of any resource-permission story.
- Also possible: GRUB's `devicetree` command mis-relocating/mis-sizing the
  larger FDT (there's a GRUB `fdt resize` step in the TI bootscript flow
  found in `u-boot-bb.org-initial-env` — see below — that GRUB's own
  `devicetree` command may not replicate).

**Likely the real fix: this board isn't meant to boot through GRUB's
raw `devicetree` command at all.** `u-boot-bb.org-initial-env` (this board's
compiled-in default U-Boot env, dumped from the deploy dir) has a whole
`bootcmd_ti_mmc` / `get_fdt_mmc` / `get_overlay_mmc` / `name_overlays` flow —
TI's standard pattern of U-Boot loading the **kernel's own** dtb from
`/dtb/${fdtfile}` (separate from U-Boot's internal DM dtb), applying
`.dtbo` overlays via `fdt apply` (with an explicit `fdt resize` first) via
`name_overlays`, and booting via `booti`/FIT rather than through
GRUB/EFI at all. This board's `.wks`/grub.cfg (inherited from an earlier
board's config, which normally boots via Falcon and may never exercise this
GRUB path for real) bypasses that entire intended mechanism. The likely real fix
is getting this board onto that native TI boot flow — or replicating its
`fdt resize`-before-`fdt apply` sequence — rather than a raw GRUB
`devicetree` command, but this needs more research before another live
attempt: **each failed attempt costs a full power cycle and physical SD-card
swap, and a hang is a strictly worse failure mode than the crash loop
(unreachable over both serial and network) — don't rush the next attempt.**

Current state: reverted to the known-stable config (crash loop masked, no
HDMI, no devicetree line) — confirmed this is the safe baseline to leave the
board in between investigation sessions.

### Resolved (2026-08-29, later same day): the "hang" was never a hang — display works end to end

**The earlier hang diagnosis was wrong, and it's important to say so
plainly.** Root cause: the manual test that appeared to hang omitted
`bootargs` entirely (no `console=`, no `earlycon`). The kernel was booting
*fine* — it just had no configured console, so it produced zero UART output
and looked identical to a real freeze. This was confirmed two ways before
trusting it: (1) network reachability was checked immediately after the
apparent "hang" and found genuinely down at the time (real hang, that
specific run) but (2) a repeat with explicit `console=ttyS2,115200n8
earlycon` added to `bootargs` booted completely cleanly with full output.
**There is no SYSFW/TISCI resource-permission issue, no earlycon/pinmux
mismatch, and no FDT-relocation bug** — all three candidate causes floated
above were wrong. Flagging this misdiagnosis explicitly since it briefly
looked like a strong argument for deferring Falcon further; it wasn't.

**Verification method: manual interactive U-Boot boot over serial, driven
entirely from the Mac.** Interrupting autoboot by hand (racing the 2-second
countdown) is unreliable over a chat-driven session, so instead: trigger
`reboot` over SSH from the Mac and *simultaneously* start sending repeated
space characters over the serial TTY (`printf ' ' > /dev/cu.usbmodem102`)
in a loop — fully software-timed, no human reaction time in the loop, lands
in the autoboot window reliably. From the `=>` prompt: `load mmc 1:1
${fdtaddr} k3-am67a-beagley-ai.dtb`, `load mmc 1:1 ${kernel_addr_r} Image`,
`setenv bootargs '...'`, `booti ${kernel_addr_r} - ${fdtaddr}`. The dtb file
itself was pushed onto the already-RW-mounted boot partition over `scp`
while Linux was running (`/run/media/boot-mmcblk1p1/`, confirmed mounted rw
by the stock image) — **no SD card was ever removed from the board for any
of this**, including editing `grub.cfg` itself over SSH. Worth remembering
for all future iteration on this board: the boot partition is writable live
over the network, so a card pull is essentially never necessary once SSH is
up.

**Network boot (TFTP) was attempted first and is a dead end on this LAN —
not a fix, don't retry it here.** Set up macOS's built-in `tftpd`
(`/System/Library/LaunchDaemons/tftp.plist`, serves `/private/tftpboot`),
confirmed it listening (`lsof -i UDP:69`), pushed `Image`/`.dtb` there. Board
never got past ARP: `tcpdump -i en0 arp` during a live `tftpboot` attempt
showed **zero** ARP request packets from the board's temporary static IP
arriving at the Mac's NIC at all (confirmed via packet capture, not
inferred) — while normal Linux-side traffic between the same two devices
(SSH, ping) works fine in the other direction. Likely cause: this LAN is a
mesh WiFi system (`zenwifi_axe7800`) and the board's wired Ethernet port may
be landing on a different mesh node/segment with client/AP isolation
between it and the Mac — environmental, not fixable from the Mac side.
Fell back to loading via `load mmc` from the SD card instead (see above),
which is nearly as convenient since the card never has to leave the board.

**The actual fix, now permanent in `grub.cfg`:**
```
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
default=boot
timeout=3
menuentry 'boot'{
devicetree /k3-am67a-beagley-ai.dtb
linux /Image root=PARTUUID=076c4a2a-02 rootwait rootfstype=ext4 ro console=ttyS2,115200n8 earlycon
}
```
Two changes from stock: the `devicetree` line (so the kernel gets the real
`main_i2c1`/`it66122`-enabled tree instead of U-Boot's stripped internal
one — see above), and `console=ttyS2,115200n8 earlycon` (without which the
correct boot is silent and looks hung, per the misdiagnosis above). Rootfs
kept `ro`, matching this project's standard read-only-root convention — the
one manual test run used `rw` by mistake, not carried forward.
`systemd.mask=...` for `ultima-app`/`ultima-splash` **removed** — no longer
needed, both units now succeed.

**Confirmed end to end, twice, including one fully unattended reboot (no
manual U-Boot intervention, autoboot straight through) with the permanent
`grub.cfg` in place:**
- `main_i2c1` registers: `omap_i2c 20010000.i2c: bus 3 rev0.12 at 400 kHz`
- `it66121 3-004c: IT66121 revision 0 probed` — HDMI bridge chip alive
- `tidss 30220000.dss: [drm] Initialized tidss 1.0.0` and `[drm] fb0:
  tidssdrmfb frame buffer device` — `/dev/fb0` exists, `/dev/dri/card0`
  and `renderD128` also present
- `ultima-splash.service` succeeds (previously failed every time)
- `ultima-app.service` starts clean and **renders real frames**:
  `[6.82] first frame rendered (afterRendering)`, `first frame swapped`,
  total app startup ~1.2s — running on `linuxfb` (not yet flipped to
  `eglfs`, see below), stable for the full observed runtime, zero crashes,
  zero `sig=6` audit lines in the fresh boot's log
- Board recovered from every earlier scare (real hang from the accidental
  no-`bootargs` test, the actual crash loop) with nothing worse than a
  power cycle each time — no repeat of the storage-corruption-style scare
  from earlier in this project's history

**This is milestone 1's actual finish line**: `ultima-app` runs and renders
on real BeagleY-AI hardware for the first time, unattended boot, no masks,
no manual steps. Not done as part of this: flipping `QT_QPA_PLATFORM` from
`linuxfb` to `eglfs`/`eglfs_kms` now that `/dev/dri/card0` is confirmed
present (the GPU driver risk flagged earlier in this section didn't
materialize at build time; runtime is now worth trying since the DRM/KMS
side is proven working) — worthwhile next step, not required for a working
dash.

**Falcon status, revisited**: the specific reason cited earlier for
deferring Falcon — "given a board isn't even in hand yet" and "wait for a
confirmed stock hardware boot" — no longer applies; both conditions are now
met. The underlying difficulty assessment from the feasibility check
earlier in this section (bb.org's tree has zero Falcon code at all, HS-FS
is separately untested territory, real transplant work not a small config
change) is unaffected by anything found this session and still stands as
the actual scope estimate for that work.

### The display fix, baked into the Yocto build (2026-08-29, later same day)

Everything in the section above was applied by hand, live, over SSH/serial —
real on the physical card, but not reproducible from a fresh `./build.sh`.
Folded both fixes into `beagley-ai/meta-ultima-beagley-ai-src/wic/ultima-beagley-ai.wks.in`
itself — no separate `.bbappend` or `IMAGE_BOOT_FILES` needed, both turned
out to be controllable from the wks file alone once the actual mechanism was
read (oe-core's `scripts/lib/wic/plugins/source/bootimg-efi.py`, not
guessed):

- **`part --source bootimg-efi --sourceparams="loader=${EFI_PROVIDER},dtb=k3-am67a-beagley-ai.dtb"`**
  — the `dtb=` sourceparam is `bootimg-efi`'s own built-in mechanism for
  exactly this: `_copy_additional_files()` copies `$DEPLOY_DIR_IMAGE/<dtb>`
  onto the FAT partition, and `do_configure_grubefi()` appends a
  `devicetree /<dtb>` line to the generated `grub.cfg` — both problems (dtb
  missing from p1, grub.cfg not referencing it) from one parameter. No
  `IMAGE_BOOT_FILES` involved at all; that variable exists in the same
  plugin (`do_configure_partition`) for a different purpose (arbitrary
  extra files onto the ESP), overkill and unnecessary here.
- **`bootloader --append="rootfstype=ext4 console=ttyS2,115200n8 earlycon ${TI_WKS_BOOTLOADER_APPEND}"`**
  — `bootloader.append` from the wks file is what `do_configure_grubefi`
  puts on the `linux` line's cmdline. `TI_WKS_BOOTLOADER_APPEND` itself
  (`meta-ti-bsp/conf/machine/include/k3.inc`) defaults to empty for the
  j722s/k3 family — only `am64xx.inc` hardcodes `console=ttyS2,115200n8`
  for its own machines, confirmed by grepping every definition of that
  variable across `tisdk/sources/`. Added the console/earlycon args
  directly in this layer's own wks file instead of trying to override the
  shared TI variable from a machine conf this layer doesn't own — simpler
  and fully localized to this board.

**Rebuilt and verified against the actual artifact, not assumed:**
`BOARD=beagley-ai ./build.sh tisdk-base-image` — 8918/8918 tasks, all
succeeded (mostly served from sstate, as expected for a wic/wks-only
change). Extracted p1 from the freshly built `.wic.xz` the same way as
earlier this session (`dd`+`mtools`, no loop device — still unavailable in
this Docker/OrbStack setup): `k3-am67a-beagley-ai.dtb` (97448 bytes, exact
match to the deploy dir's copy) is now present at the FAT partition root,
and `grub.cfg` reads:
```
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
default=boot
timeout=3
menuentry 'boot'{
linux /Image root=PARTUUID=076c4a2a-02 rootwait rootfstype=ext4 console=ttyS2,115200n8 earlycon  ro
devicetree /k3-am67a-beagley-ai.dtb
}
```
**One difference from the hand-verified config worth flagging, not yet
re-confirmed on hardware**: the plugin always emits `linux` before
`devicetree` (see `do_configure_grubefi`'s fixed write order) — the manual
fix applied directly to the card earlier this session had `devicetree`
*before* `linux`. GRUB's `linux`/`devicetree`/`initrd` commands are loader
*setup* calls (actual boot happens once the menuentry script finishes, not
on the `linux` call itself), so this ordering is expected to be equivalent
and is exactly what every other OE machine using `bootimg-efi`'s `dtb=`
sourceparam already relies on — but this specific generated `grub.cfg` has
not itself been booted on the physical board yet. Do a normal (unattended)
boot with a freshly flashed card from this build to close that loop before
treating milestone 1 as fully done from a clean build, not just from the
hand-patched card.

Not done as part of this: reflashing/rebooting the physical board — this
was a build-only change, verified by inspecting the `.wic` artifact
directly, not by touching hardware again this round.

### Clean-build boot confirmed on hardware (2026-08-29, later same day) — milestone 1 actually done

Closed the loop flagged just above. Pulled the rebuilt image fresh from the
Docker volume (the local `deploy-beagley-ai/` copy was stale — timestamped
before the rebuild, would have re-flashed the pre-fix image), reflashed the
SD card (`./flash.sh disk4 ...`), and did a fully clean
boot: SSH `poweroff`, confirmed the board actually went dark (dropped off
`ping`, not just the SSH session closing) before pulling the card, no
manual U-Boot intervention on power-on.

**The `linux`-before-`devicetree` ordering question is resolved: it doesn't
matter.** On-device `grub.cfg` confirms the plugin's write order made it to
the card unchanged, and it booted clean anyway — GRUB's `linux`/`devicetree`
calls are load-only, execution happens after the menuentry script finishes,
so order between them was never actually significant. No longer a caveat.

**Verified via SSH afterward, not just serial:**
- `dmesg`: `it66121 3-004c: IT66121 revision 0 probed` at t=2.9s, `tidss
  30220000.dss: [drm] fb0: tidssdrmfb frame buffer device` at t=3.3s. (The
  intervening "[drm] Cannot find any crtc or sizes" lines are transient
  probe-order noise, not a fault — `/dev/fb0` shows up two lines later.)
- `systemctl show ultima-app ultima-splash -p NRestarts` → `NRestarts=0` for
  both — not just "active" at a point in time, actually never crashed.
- `journalctl -k | grep -c sig=6` → `0`.
- `ultima-app` log: QML loaded, first frame rendered, first frame swapped,
  all within 1.42s of app start — same clean startup profile as the
  hand-patched card.
- Unrelated pre-existing failures also present (`e5010` IRQ registration,
  `cc33xx` wifi firmware missing) — expected on this dev image, nothing to
  do with display, not a regression from this change.

This is the actual, reproducible-from-clean-build finish line for milestone
1 — not just the earlier hand-patched-card result. A fresh `git clone` +
`./build.sh tisdk-base-image` + `./flash.sh` now produces a card that boots
straight to a working gauge-cluster display, no manual grub.cfg surgery
required.

### Touchscreen didn't work — root cause was USB entirely disabled, not a touch driver problem (2026-08-29, later same day)

With display working, the Waveshare USB-HID touchscreen (the same panel/model
this project used before — `hid-multitouch`, USB `0712:000A`) still didn't
respond. First check ruled out the obvious: this
isn't a touch-specific problem at all.

**`/sys/bus/usb/devices` was completely empty — not even a root hub.** A
merely-unplugged touchscreen still leaves a root hub; zero entries plus zero
`xhci`/`dwc3`/`cdns` lines anywhere in `dmesg` means the SoC's USB
controller itself never probed. `lsmod` and `/proc/config.gz` confirmed the
driver support is all there (`CONFIG_USB_HID=y`, `CONFIG_HID_MULTITOUCH=m`,
cdns3 built in) — this is a devicetree-enablement gap, not a missing driver.

**Confirmed via the actual dts, not guessed:** BeagleY-AI's board dts
(`k3-am67a-beagley-ai.dts`, upstream BeagleBoard.org kernel source, not this
project's own) never references `usb0`/`usb1`/`&usb` anywhere. The SoC dtsi
(`k3-j722s-main.dtsi`) defines the SoC's only USB3 host/OTG controller
(`usbss1`/`usb1`, `cdns,usb3`, feeding this board's 4 physical USB-A ports
via an onboard hub) with
`status = "disabled"`, and the board never turns it on.

**Second layer, found by actually trying the obvious fix and watching it
fail correctly:** enabling `usbss1`/`usb1` alone left the controller
permanently stuck — `dmesg`: `platform 31200000.usb: deferred probe
pending: cdns-usb3: Failed to get cdn3,usb3-phy`, confirmed non-transient
via `/sys/kernel/debug/devices_deferred` after the board had been up over a
minute. Root cause: `usb1`'s phy comes from `serdes0` (a SERDES lane
already correctly configured for USB3 by this board's own dts —
`serdes_ln_ctrl`'s `idle-states` already routes SERDES0 lane0 to USB, and
`&serdes0 { status = "okay"; serdes0_usb_link { cdns,phy-type =
<PHY_TYPE_USB3>; ...} }` is already present), but `serdes0`'s *parent* HW
block, `serdes_wiz0` (the Cadence Torrent SerDes wrapper itself,
`ti,am64-wiz-10g`), defaults to `status = "disabled"` in the SoC dtsi and —
same pattern as usb1 — is never enabled by the board dts either. Nothing
downstream of a disabled parent can probe no matter how correctly the child
is configured. (`serdes_wiz1`, PCIe's copy of the same block, has the
identical gap — confirmed via the pre-existing, harmless-looking `j721e-pcie:
Failed to init phy` deferred-probe line present since this board's very
first boot. Not fixed here, same root cause, out of scope — flagging for
whoever picks up PCIe.)

**The fix, both pieces, ported from TI's own reference:** TI's J722S EVM
board dts (`k3-j722s-evm.dts`, same SoC) already carries the working
pattern — enable `serdes_wiz0`, then `usbss1`/`usb1` with a dedicated
`pinctrl-single` entry for `USB1_DRVVBUS` (`J722S_IOPAD(0x0258, PIN_INPUT,
0)` — a **fixed SoC pin**, not board-routed, confirmed unused anywhere else
in this board's own dts, so no BeagleY-AI-specific schematic lookup was
needed) plus the already-present `serdes0_usb_link` phy reference. Ported
verbatim, not invented.

**Verified live on hardware before touching any Yocto source** (same
discipline as the grub.cfg display fix — iterate on the live artifact, bake
in only after proof): the board's own `k3-am67a-beagley-ai.dtb` already has
a `__symbols__` table (confirmed: `usbss1`, `usb1`, `serdes0_usb_link`,
`main_pmx0` all present as exported labels), so the fix was written as a
standalone overlay (`/plugin/`, raw pinctrl cell values instead of the
`J722S_IOPAD`/`PIN_INPUT` macros since a standalone `dtc` compile has no
kernel-tree include path), applied to the deployed dtb with `fdtoverlay`
(both `dtc`/`fdtoverlay` already available via Homebrew, no container
needed), pushed over SSH, board rebooted. Confirmed working:

```
xhci-hcd xhci-hcd.5.auto: xHCI Host Controller
xhci-hcd xhci-hcd.5.auto: Host supports USB 3.0 SuperSpeed
hub 1-1:1.0: USB hub found
hub 1-1:1.0: 4 ports detected
usb 1-1.3: New USB device found, idVendor=0712, idProduct=000a
input: Waveshare  Waveshare  Touchscreen as .../input/input0
hid-multitouch 0003:0712:000A.0001: input,hiddev0,hidraw0: ...
```

`/dev/input/event0` present, `ultima-app` (still `linuxfb`) picked up touch
with **no extra Qt configuration at all** — no `QT_QPA_GENERIC_PLUGINS`, no
service env changes. `libqevdevtouchplugin.so` is already packaged
(confirmed present at `/usr/lib/plugins/generic/` before assuming a rebuild
would be needed), and Qt5's udev-based generic-plugin auto-detection loads
it on its own regardless of platform backend — this turned out not to be
the linuxfb-vs-eglfs gap it looked like it might be going in. Confirmed
functionally, not just at the kernel level: tapping the car (see
`main.qml`'s touch-feedback-dot / `Camera360Screen` binding in
`GAUGE-CLUSTER.md`) produced `[camerafeed] open(/dev/mycam/cam4): No such
file or directory` in `ultima-app`'s own log — the camera-open failure is
expected (no physical cameras on this bench setup), but it proves the tap
was received, routed through QML, and drove real app behavior.

**Baked into the Yocto build**, same pattern as the kernel `.cfg` fragments
this project already carries: a plain unified-diff patch
(`beagley-ai/meta-ultima-beagley-ai-src/recipes-kernel/linux/linux-bb.org/0001-arm64-dts-k3-am67a-beagley-ai-enable-usb1-host.patch`)
appending the four enable blocks to the end of the upstream board dts, wired
via `linux-bb.org_%.bbappend`'s `SRC_URI:append` (mirrors the kernel
`SRC_URI:append` patch pattern this project already uses — no
`KERNEL_CONFIG_FRAGMENTS` needed here since this is a dts patch, not a
Kconfig fragment). Test-applied against the actual upstream source tree
with `patch -p1 --dry-run` before wiring it in (clean, 2-line offset, no
fuzz) — not assumed to apply just because it matched by inspection.

### Falcon boot mode for BeagleY-AI — feasibility check (2026-08-28, later same day)

Went looking at what a Falcon fork for j722s would actually involve, before
attempting it. Conclusion: **this is a materially bigger job than an
earlier board's Falcon port was, not attempted this session, and needs a scope/
priority call from the user before someone sinks real hours into it.**

**The gap is a missing feature, not a missing wire-up.** An earlier board's
Falcon port worked because its `u-boot-ti-staging` already had a *complete*
Falcon implementation in `arch/arm/mach-k3/common.c` — `spl_start_uboot()`,
`k3_falcon_prep()`, `k3_falcon_fdt_fixup()` (confirmed present, lines 581–710
of that file in the 2025.01+git TI-staging tree) — just never wired up for a
non-TI-EVM board name. That fork was three small patches plus a
`UBOOT_CONFIG_FRAGMENTS` line scoping an *existing* TI feature to a new board.

BeagleY-AI's bootloader is different at the recipe level:
`beagle-bsp.inc`'s `bb_org` BSP variant builds `u-boot-bb.org`
(`git://github.com/beagleboard/u-boot.git`, BeagleBoard.org's own
independently-maintained fork, U-Boot 2025.10), not `u-boot-ti-staging`
(`git.ti.com/git/ti-u-boot/ti-u-boot.git`, TI's own fork, U-Boot 2025.01) —
confirmed via each recipe's `SRC_URI`/`UBOOT_GIT_URI`. Grepping the actual
unpacked `u-boot-bb.org` 2025.10 source tree built for `beagley-ai` this
session: **zero occurrences of `falcon` anywhere under `arch/arm/mach-k3`**,
and zero occurrences of `spl_start_uboot`/`CONFIG_SPL_OS_BOOT`/`k3_falcon` in
its `mach-k3/common.c` — the entire Falcon SPL subsystem TI's fork carries is
simply absent, not just unreferenced by a board config. What's established:
`k3_falcon_*` is a TI-downstream-only feature — present in TI's own
`ti-u-boot`, absent from bb.org's *newer* (2025.10 vs 2025.01) tree, so it
isn't upstream code bb.org would have inherited for free. What's genuinely
**not** established: how much `mach-k3/common.c` has otherwise diverged
between the two trees — bb.org's `mach-k3` is itself clearly TI-derived K3
support (`j722s/`, `sysfw-loader.h`, `security.c`, the same HS-FS machinery),
so this may be closer to a conflict-heavy cherry-pick than a from-scratch
transplant, or it may not — that divergence is unmeasured and is the first
thing whoever picks this up should check, not something to assume either way.
Either way it's transplanting TI's downstream `k3_falcon_*` C code (plus its
Kconfig options, the `k3_r5_falcon.config` fragment, and the `tifalcon.bin`
binman node) onto a tree that has evolved separately for ~9 months of
upstream U-Boot, with no guarantee the functions Falcon hooks into still
match shape.

**Compounded by HS-FS.** `beagley-ai-k3r5.conf` builds the HS-FS (not GP)
signed boot chain (see "The R5/TIFS boot chain is HS-FS" above) — genuinely
new territory for this project. Falcon means rebuilding the R5 SPL with a
different config fragment; whether meta-beagle's default/dev signing still
produces a chain the ROM accepts once that SPL is Falcon-modified is an open
question with no way to check without deep TI secure-boot documentation
reading, let alone hardware.

**Not attempted, and shouldn't be, without a board.** Even a syntactically
clean patch set here would be unverifiable — Falcon bugs manifest as a
completely dead boot (no U-Boot console, no kernel log), and this project has
already hit one real storage-corruption scare from a *known-working* config
misstep; doing first-time HS-FS + Falcon
debugging blind, with no serial-console ground truth to fall back on, is a
bad risk trade until hardware is in hand anyway.

**Recommendation, not a decision made here:** Falcon is a boot-time
optimization — valuable, but the actual stated reason for this whole
BeagleY-AI port is the Wave5 hardware encoder for dashcam recording, which
doesn't need Falcon at all. Given a board
isn't even in hand yet, the camera driver port and CAN bring-up (both also
deferred, see below) deliver more toward that actual goal per hour spent than
a from-scratch Falcon backport with no way to verify it. Suggest deferring
Falcon until after first hardware boot (stock, non-Falcon) is confirmed
working, and revisiting priority then — flagging it here rather than
defaulting to "keep going."

### Falcon on J722S/BeagleY-AI: dead end, not just deferred (2026-08-29, later same day)

Hardware is now in hand and both the display and USB1/touchscreen fixes are
verified and baked in (see the two sections above this one). Picked Falcon
back up to actually resolve the "materially bigger job" open question from
the feasibility check above, by diffing the real fetched `u-boot-bb.org`
2025.10 source against `u-boot-ti-staging` 2025.01 (both already present in
the Docker build volume from this project's own builds) instead of reasoning
from docs. Conclusion: **the C-code/binman gap is real but portable — the
actual blocker is one level lower, and it's final.**

**What turned out to be fine (contrary to the "conflict-heavy cherry-pick"
worry above):** the falcon SPL logic in TI staging's `common.c`/`r5/common.c`
(~150 lines: `spl_start_uboot()`, `k3_falcon_prep()`,
`k3_falcon_fdt_fixup()`, `k3_r5_falcon_bootmode()`) turned out to be
self-contained — every API it calls (`spl_start_uboot` as a weak SPL hook,
`struct spl_image_loader`, `spl_image_fdt_addr`, `spl_loader_name`) exists
verbatim in bb.org's tree, and it doesn't touch the ~250 unrelated lines of
TI-staging-only LPM/wake and clock-fixup code also in that file. Likewise the
binman side: TI staging's `ti_falcon_template` (in the shared, SoC-generic
`k3-binman.dtsi`) is absent from bb.org's copy of that file, but bb.org's own
`k3-am67a-beagley-ai-u-boot.dtsi` already uses the identical
`insert-template = <&ti_spl_template>` pattern for its normal (non-falcon)
boot, including the exact same J722S DM firmware
(`ti-dm/j722s/ipc_echo_testb_mcu1_0_release_strip.xer5f`) and `custMpk.pem`
signing convention TI's falcon template needs — both already fetched and
working in this board's current boot. None of that would have been a
blocker.

**What is a blocker: TI has never published the TIFS stub firmware J722S's
Falcon fast-path needs.** Both trees' falcon FIT images load a
`tifsstub-<gp/hs/fs>` image alongside ATF/OP-TEE/DM — this is what lets the
R5 SPL do falcon's abbreviated security handshake without the normal A53-SPL
chain. That binary comes from TI's `ti-linux-firmware` git repo via the
existing `ti-sci-fw` recipe (`meta-ti-bsp/recipes-bsp/ti-sci-fw/`, already
built for this board's normal boot — its `do_deploy:k3r5()` unconditionally
installs `ti-fs-stub-firmware-*` from that repo, whatever exists). Checked
the actual fetched repo (`tisdk/downloads/git2/git.ti.com.git.processor-
firmware.ti-linux-firmware.git`) directly: **`ti-fs-stub-firmware-*` exists
only for `am62x`/`am62ax`/`am62px`** — exactly the four machines TI's own
`ti-falcon` Yocto override already covers (am62xx-evm, am62axx-evm,
am62pxx-evm, am62xx-lp-evm). Zero files matching `j722s`/`am67a` on the
pinned SRCREV. Re-checked live against a fresh `git fetch` of every branch
TI publishes (`main`, `ti-linux-firmware-6.12.y-next`, etc.) — the `-next`
branch's tip commit was 12 days old at the time of this check and had just
touched J722S IPC firmware in the same commit, so this isn't a stale-mirror
artifact. Still nothing for J722S on any branch.

**So this isn't "TI hasn't wired the config yet," it's "TI hasn't shipped
the firmware yet."** No amount of backporting C code or binman dtsi nodes
fixes a missing signed firmware blob only TI can produce. Falcon is off the
table for this board until TI publishes J722S stub firmware upstream — worth
periodically re-checking (`git fetch` that repo, grep for
`ti-fs-stub-firmware-j722s` across branches), but not worth further
engineering time until that happens. This also means the earlier
"family-capable... reachable" framing for Falcon was wrong on this specific
point — the SPL bootflow being shared across the
am62xxx-extended family (true, per U-Boot's own J722S board doc) does not
imply TI has released the same firmware for every member of that family.

**With Falcon closed off, the GRUB/console-spam lever is not throwaway
anymore.** Earlier framing was "don't bother trimming GRUB, Falcon deletes
that path entirely" — that's now moot. `ultima-beagley-ai.wks.in` already
carries `console=ttyS2,115200n8` (needed for the display fix) with no
`quiet`, the same synchronous-UART-spam shape an earlier board measured at
~3.3s/~38KB of console text before its own `quiet` fix. Added `quiet` and
`vt.global_cursor_default=0` to the bootloader append (same reasoning as that
earlier falcon-cmdline fix) — not yet re-measured on this board's own
boot-log capture, so treat as applied-by-analogy until confirmed with a real
serial capture.

### Deferred (not this milestone)

Falcon boot (see feasibility check just above — bigger than expected, defer/
proceed is a call for the user), the MY-CAM004M camera driver port (open
question: J722S CSI-2 receiver multi-virtual-channel demux for 4 simultaneous
1080p streams — the same open question already flagged for this exact board),
CAN bring-up (Waveshare
MCP2515 HAT on the real MCU_SPI0, not the header's default software-SPI
shim), WiFi, RTC, and — obviously —
anything requiring the physical board: flashing, serial verification, and the
eglfs switch-back.

### Falcon on BeagleY-AI: working (2026-08-30) — the "dead end" above was wrong

**Status: Falcon boots on the BeagleY-AI, hardware-verified, first attempt.**
Chain is now ROM → R5 SPL (`tiboot3.bin`) → ATF → OP-TEE → kernel. No A53
SPL, no U-Boot proper, no GRUB. Measured on the bench (timed serial capture,
`reboot` from a running system, times from first byte after reset):

| event (serial, s after first byte)   | stock GRUB chain | falcon |
|--------------------------------------|-----------------:|-------:|
| R5 SPL banner                        |             0.92 |   0.96 |
| "Starting ATF on ARM64 core"         |             1.16 |   3.02 |
| A53 SPL banner                       |             1.43 |      — |
| GRUB menu (then 3 s `timeout=3`)     |             6.07 |      — |
| kernel time zero (derived)           |            ~10.9 |   ~3.4 |
| **ultima-app first frame rendered**  |        **16.55** | **10.16** |
| (kernel-relative first frame)        |             5.64 |   6.73 |

Logs: `boot-logs/beagley-ai-stock-grub-reboot-timed-20260830T101826.txt` and
`boot-logs/beagley-ai-falcon-reboot-timed-20260830T102001.txt`, same board,
same card, consecutive reboots (`bench-compare` swap of `tiboot3.bin` only).
**−6.4 s, 39 % of the total.** Both runs already had `quiet` on the cmdline.
Where the stock chain's time went: U-Boot proper alone 1.4 → 6.0 s (its
own probing, env, EFI), then GRUB's 3 s menu timeout plus ~1.9 s to load the
40 MiB `Image` through the EFI stub. Falcon's ~2 s between the SPL banner
and ATF is the R5 SPL streaming that same `Image` off the SD card (see the
"next lever" note at the end). One oddity worth a look later: kernel-relative
time to first frame is ~1.1 s *longer* under falcon (6.73 vs 5.64 s) — the
kernel starts ~7 s earlier in wall time, so something it waits on (a PHY,
DM/TIFS, the SD card re-init that U-Boot proper used to have done already)
is plausibly on the critical path now. Not chased; falcon still wins by a
wide margin and that's a kernel-side investigation, not a bootloader one.

The first falcon boot ever on this board (bring-up log
`boot-logs/beagley-ai-falcon-bringup-20260830T100742.txt`) came up
identically — 10.20 s to first frame — so the number is repeatable, not a
warm-cache fluke.

The section directly above concluded Falcon was blocked on a TIFS stub
firmware TI never published for J722S. That was a misread of what the stub
is for. Two independent data points killed it: (1) a TI E2E user got R5-SPL
→ ATF → OP-TEE → Linux booting on a **J722S HS-FS EVM** (thread 1376134,
SDK 9.2) with a hand-built FIT and no stub, following the recipe TI's own
Keerthy J posted for J721S2 (thread 1334006); (2) TI's MCU+ SDK `main`
grew `FALCON_MODE=1` support for `j722s-evm` in March 2026
(`tools/boot/linuxAppimageGen/board/j722s-evm/config.mak`, examples
`sbl_sd_hlos`) — an SBL-based variant, also stub-free. The `tifsstub-*`
image in TI-staging's `ti_falcon_template` mirrors what AM62x's *normal*
`tispl` template carries for low-power-mode resume; this board's normal
boot already runs without one, so its falcon path doesn't need one either.

**How it works here (no falcon code at all — this is the whole trick).**
bb.org's stock R5 SPL already loads a FIT whose `firmware` is ATF and whose
`loadables` are OP-TEE + the DM firmware, then starts ATF on the A53 and
drops into the DM itself (`arch/arm/mach-k3/r5/common.c` `jump_to_image`).
`tifalcon.bin` is that same FIT with the A53 SPL swapped for the kernel
`Image` and the DTB as two more plain loadables (`type = "standalone"`),
placed exactly where TF-A's compiled-in constants make it jump:
`PRELOADED_BL33_BASE` (BL33 = the kernel) and `K3_HW_CONFIG_BASE` (x0 =
DTB). ATF → OP-TEE → kernel is then the normal TF-A flow; the R5 SPL never
knows a kernel was involved. `common/spl/spl_fit.c` only appends/fixes up
an FDT for `os = u-boot` (or `linux` under `SPL_OS_BOOT`, unset here) so
neither image is touched, and on this HS-FS part unsigned images just get
the "Did not detect image signing certificate. Skipping authentication"
warning (`arch/arm/mach-k3/security.c`, one per image, five in the log).
Nothing about this is J722S-specific beyond addresses.

**Memory map** (the only thing that had to change; DDR starts at
`0x80000000`, the board has 4 GiB):

| addr         | what                     | set by                                  |
|--------------|--------------------------|-----------------------------------------|
| `0x80000000` | ATF                      | `CONFIG_K3_ATF_LOAD_ADDR` (unchanged)   |
| `0x80080000` | FIT metadata buffer      | `CONFIG_SPL_LOAD_FIT_ADDRESS` (unchanged) |
| `0x82000000` | kernel `Image` (~43 MiB incl. BSS) | TF-A `PRELOADED_BL33_BASE` (was `0x80080000`) |
| `0x88000000` | kernel DTB               | TF-A `K3_HW_CONFIG_BASE` (was `0x82000000`) |
| `0x89000000` | DM firmware staging      | binman default (unchanged)              |
| `0x8c000000` | R5 SPL relocated stack   | `CONFIG_SPL_STACK_R_ADDR` (was `0x82000000` = kernel) |
| `0x9e800000` | OP-TEE                   | `CONFIG_K3_OPTEE_LOAD_ADDR` (unchanged) |

TF-A's defaults (BL33 `0x80080000`, DTB `0x82000000`) are what TI's MCU+
SDK uses for J722S, but they assume a kernel under ~31 MiB — this build's
uncompressed `Image` is 40 MiB on disk (42,025,472 B, `image_size` in the
arm64 header 0x2950000) and would overrun the DTB slot. The two TF-A
overrides are the exact pair meta-ti's `ti-falcon` DISTROOVERRIDE applies
for TI's own EVMs (`trusted-firmware-a-ti.inc`), just scoped to
`beagley-ai` here. OP-TEE is built `CFG_DT=n`/`CFG_EXTERNAL_DT=n` and never
touches the DTB, so nothing else cares about the move.

**Two DTB fixups U-Boot proper used to do at runtime, now baked at build
time** (`recipes-bsp/ultima-falcon-fit`, `fdtput` on the deployed DTB):

1. `/chosen/bootargs` — nothing populates it anymore. `root=/dev/mmcblk1p2`
   rather than `PARTUUID=`: only one storage device on this board, and the
   MBR disk signature is assigned by wic per build and re-patched by
   `flash.sh`, so a build-time DTB can't know it. (The bench test used the
   card's live PARTUUID; both verified.)
2. `/reserved-memory/tfa@9e780000` `reg` → `0x80000000`/`0x80000`. The
   upstream DT reserves ATF at `0x9e780000`; bb.org actually loads it at
   `0x80000000` and U-Boot proper rewrites the node in `ft_system_setup`
   (`arch/arm/mach-k3/j722s/j722s_fdt.c`). **Under GRUB this mismatch was
   silently masked**: the kernel was booting via the EFI stub, taking
   memory from U-Boot's UEFI memory map (which reserved ATF's real location)
   and ignoring the DT's `reserved-memory` — the live kernel's DT still said
   `tfa@9e780000` while ATF sat at `0x80000000`. With Falcon the DT is
   authoritative; without this fixup the kernel would hand
   `0x80000000-0x80080000` out as RAM and corrupt the running ATF.

Things checked on the falcon-booted system and found fine without any
U-Boot help: Ethernet MAC (same `04:25:e8:...`, `am65-cpsw-nuss` reads it
from efuse via `ti,syscon-efuse`; DHCP gave the same IP), `MemTotal` 3.78
GiB from the DT `memory` node, `/dev/fb0` + both DRM cards, `ultima-app`
active, `optee@9e800000` reservation (already correct in the upstream DT).

**What was built (all in `beagley-ai/meta-ultima-beagley-ai-src`):**

- `recipes-bsp/trusted-firmware-a/trusted-firmware-a_%.bbappend` —
  `EXTRA_OEMAKE:append:beagley-ai = " PRELOADED_BL33_BASE=0x82000000
  K3_HW_CONFIG_BASE=0x88000000"`.
- `recipes-bsp/u-boot/u-boot-bb.org_%.bbappend` +
  `u-boot-bb.org/am67a_beagley_ai_r5_falcon.config` —
  `UBOOT_CONFIG_FRAGMENTS:beagley-ai-k3r5`, copied into `${S}/configs/`
  in `do_configure:prepend` (same lesson the earlier Falcon work learned:
  the merge rule only looks there). Sets `CONFIG_SPL_STACK_R_ADDR=0x8c000000`
  and `CONFIG_SPL_FS_LOAD_PAYLOAD_NAME="tifalcon.bin"` — the payload rename
  is what lets the stock `tispl.bin` stay on the card untouched so
  swapping `tiboot3.bin` alone selects the mode.
- `recipes-bsp/ultima-falcon-fit/` — assembles `tifalcon.bin` from
  `DEPLOY_DIR_IMAGE` (`bl31.bin`, `optee/bl32.bin`, `ti-dm/j722s/...xer5f`,
  `Image`, the DTB) with `mkimage -f tifalcon.its -E -B 0x1000`.
  **`-E` is not optional**: the R5 SPL reads the FIT metadata to
  `0x80080000` and streams each image to its own load address; without
  external data the whole 41 MiB would land at `0x80080000`, on top of the
  kernel's slot. `do_compile[file-checksums]` on the inputs so a rebuilt
  kernel/DTB/bl31 re-assembles it instead of leaving a stale FIT in deploy.
- `wic/ultima-beagley-ai.wks.in` — p1 is now `bootimg-partition` with
  `IMAGE_BOOT_FILES:beagley-ai = "tiboot3.bin tifalcon.bin"` (set in
  `tisdk-base-image.bbappend` along with the `do_image_wic` dependency on
  `ultima-falcon-fit:do_deploy`). GRUB, `Image`, the dtb copy, `tispl.bin`
  and `u-boot.img` are gone from the card — a falcon build's own `tispl.bin`
  wouldn't work anyway (its bl31 jumps to `0x82000000`, where no A53 SPL
  is).
- `beagley-ai/falcon/build-tifalcon.sh` — bench-only: same assembly run by
  hand inside the `falcon-yocto` container against the build volume's
  deploy dir, for iterating on the FIT/DTB without a bitbake round trip.
  This is what produced the first booting image.

**Bring-up procedure that worked (for the record / next board):** rebuild
just `trusted-firmware-a` and `mc:k3r5:u-boot-bb.org` (`BOARD=beagley-ai
./build.sh "trusted-firmware-a mc:k3r5:u-boot-bb.org"`, ~2 min with sstate),
`build-tifalcon.sh`, `scp` `tifalcon.bin` + the new `tiboot3.bin` (as
`tiboot3-falcon.bin`) onto the board's mounted boot partition
(`/run/media/boot-mmcblk1p1`, rw), `cp tiboot3.bin tiboot3.bin.orig` first,
`cp tiboot3-falcon.bin tiboot3.bin; sync; reboot`. **Fallback if it doesn't
come up**: card into the Mac, `tiboot3.bin.orig` back over `tiboot3.bin` —
the stock `tispl.bin`/`u-boot.img`/GRUB set is still there on a card
flashed before this change. There is no interactive bootloader at all in
falcon mode (no U-Boot prompt, no GRUB menu); every recovery is a file swap
on the card.

**Kernel load time is the obvious next lever, not more bootloader work.**
The R5 SPL streams the 40 MiB `Image` off the SD card over the SPL's MMC
driver — about 1.8 s of the ~3 s from reset to "Starting ATF". A pruned
kernel config (this is bb.org's kitchen-sink defconfig; TI's SDK Image is
about half the size) or a faster SPL MMC mode would each cut directly into
that. Everything after "Starting ATF" is kernel + userspace and unchanged
from the GRUB chain.

**Bench mistake worth not repeating (2026-08-30, same session):** pushing a
second `tifalcon.bin` next to the first on a card that still carried the
stock files (`Image` 40 MiB + `tispl.bin`/`u-boot.img`/GRUB) left ~1 MiB
free on the 128 MiB boot partition; the `cp` over `tifalcon.bin` failed
with ENOSPC, the file was left truncated, and the reboot that followed hit
`spl_load_simple_fit: can't load image loadables index 2 (ret = -5)` →
`SPL: failed to boot from all boot devices`. In falcon mode that is the
end: no U-Boot prompt, no GRUB, no network — card out, into the Mac, fix
the file, card back, power-cycle. Rules: `df` the boot partition before
copying a FIT onto it (it needs 41 MiB free, or 82 MiB to keep the old one
as a fallback), copy to a temp name and `mv` over the live name only after
checking the size, and never script a reboot after a copy that wasn't
checked. Also: two readers on the serial tty split the bytes between them
and garble the log — kill the old `cat` before starting a new capture.

**Recipe-built FIT validated on hardware (2026-08-30, after the recovery
above).** The bitbake-produced `tifalcon.bin` (the one the wic image
carries, `root=/dev/mmcblk1p2` baked in, md5 `a7635d64…`) was put on the
card in the Mac and the board cold power-cycled: **9.26 s from first serial
byte to ultima-app's first frame** (kernel-relative 6.76 s, log
`boot-logs/beagley-ai-falcon-recipe-mmcblk1p2-timed-20260830T110338.txt`),
`/proc/cmdline` is the baked string, `/` is `/dev/mmcblk1p2` ext4 ro,
`ultima-app` active, SSH up ~22 s after power-on. The 10.16 s figure above
was a warm `reboot`; a power-cycle is ~0.9 s quicker to the SPL banner. What
is *not* yet exercised is a full `flash.sh` of a wic built with the new
`.wks.in` (the card in use was flashed pre-falcon and hand-patched) — the
partition layout and p1 contents were checked with `mdir` on the built wic
instead.

### BeagleY-AI boot-time work (2026-08-30): 9.3 s → 6.9 s cold, GPU on

Same board, same card, timed serial captures, falcon boot throughout. The
constraint was "keep the GPU and everything else the dash needs"; the first
measurement showed the board wasn't even *using* the GPU yet (the first
bring-up ran linuxfb + the software Quick backend), and turning eglfs on
made things much worse before they got better:

| power-on → ultima-app first frame           | cold  | warm `reboot` | kernel-relative |
|---------------------------------------------|------:|--------------:|----------------:|
| morning: falcon, linuxfb + software Quick   | 9.26  | 10.16         | 6.76            |
| same, eglfs_kms + PowerVR switched on       |   —   | 14.10         | 10.70           |
| **after this work, eglfs_kms + PowerVR**    | **6.94** (first boot, empty caches) | **7.53** | **5.17** (5.52 first boot) |

Warm reboots start ~0.9 s before the SPL banner (pre-reset serial noise
counts as first byte), so 7.53 warm ≈ 6.6 cold at steady state. Logs:
`boot-logs/beagley-ai-falcon-optimized-coldboot-timed-20260830T115810.txt`,
`...-optimized-reboot2-timed-20260830T120052.txt`; the GPU-on "before" is
`...-eglfs-initcalldebug-timed-20260830T111601.txt`.

**Where the time was (GPU on, before), from `initcall_debug` + the journal
+ the app's own stderr marks:** R5 SPL streaming the 41 MiB FIT at ~20 MB/s
2.1 s; kernel to init 2.3 s of which **1.27 s was a single initcall,
`init_kprobe_trace`** (kallsyms walk for the kprobe blacklist on a 40 MiB
kitchen-sink kernel — invisible in a normal dmesg, it's the silent gap
between 0.16 s and 1.44 s); systemd to app exec 1.9 s; app exec → `main()`
1.0 s; `main()` → first frame 5.4 s, of which QGuiApplication creation was
1.5–1.8 s and QML load 3.5 s. The decisive experiment: restarting the app on
the booted system — even after `echo 3 > drop_caches` — took **1.9 s** from
`main()` to first frame, not 5.4. The boot-time penalty was (a) the app
mapping **`libLLVM.so.18.1`, 100 MB** (Mesa's `gallium-llvm` PACKAGECONFIG
from arago.conf; reading it cold measured 1.03 s by itself) plus a 16 MB
`tidss_dri.so` linked against it, (b) udev coldplug replaying ~50 module
loads, two DSP + two R5 remoteproc firmware loads, WiFi, sound and video
codecs in exactly the same 5 s window, all off the same SD card, and (c)
SurroundView's GLES shader compile + four warp-mesh builds running inside
the very first frame (Qt Quick calls `updatePaintNode()` on a new
QQuickFramebufferObject even under an invisible parent). A boot with
`systemd-udev-trigger.service` masked put first frame at kernel_ts 8.5 s
instead of 10.7 — 2.2 s of pure contention.

**What changed (all in `beagley-ai/meta-ultima-beagley-ai-src` unless noted):**

- `recipes-kernel/linux/linux-bb.org/ultima-boot.cfg` (+ bbappend,
  `KERNEL_CONFIG_FRAGMENTS`): kprobes/uprobes/ftrace off, KASLR off, LSMs +
  IMA/EVM/audit off, hibernation/kexec/EFI off, PCI off (the board DT keeps
  `serdes_wiz1` disabled so j721e-pcie only ever fails and gets re-probed —
  part of `deferred_probe_initcall`'s 0.39 s), a dozen filesystems + MD/DM
  off, per-allocation debug/hardening off, `MODULE_COMPRESS` off (every
  boot-time module was being xz-decompressed), and evdev/hid-multitouch/MCAN
  built in so the touchscreen and CAN never wait on coldplug. `Image` 42.0
  → 22.2 MB, `tifalcon.bin` 43 → 23 MB, SPL→ATF 2.08 → 1.21 s, kernel to
  init 2.27 → 0.85 s. Built-in options 2325 → 1988, modules 2881 → 2692.
- `recipes-graphics/mesa/mesa-pvr_%.bbappend`: `PACKAGECONFIG:remove =
  "gallium-llvm zink virgl vulkan video-codecs"`, `GALLIUMDRIVERS = ""` (the
  recipe appends `,pvr`). No LLVM in the image; `tidss_dri.so` 16.5 → 11.4
  MB. QGuiApplication creation at boot 1.5–1.8 s → 0.41 s.
- `ultima-app/beagley-ai/ultima-app.service`: eglfs_kms + PowerVR (confirmed
  working on hardware), **`Type=notify`** — `main.cpp` sends `READY=1` from
  the first rendered frame via a hand-rolled sd_notify (no libsystemd
  dependency, no-op elsewhere) — `Nice=-10`, best-effort IO prio 0,
  `TimeoutStartSec=45` so a display-less boot still proceeds.
- `udev-trigger-after-dash.conf` → `systemd-udev-trigger.service.d/`:
  `After=ultima-app.service`. Coldplug now starts 40 ms after the first
  frame (journal: `Started Ultima gauge cluster` 5.523 s, `Starting Coldplug`
  5.561 s). udevd itself isn't delayed, so hotplug still works; everything
  the dash needs before coldplug is built in or on modules-load.d.
- `ultima-prefetch(.service/.list)`: a `cat >/dev/null` sweep of the app's
  mapped files from systemd's first transaction. A/B (masked vs not, same
  image): first frame kernel_ts 5.44 vs 5.17 — worth 0.27 s, but it only
  gets a 0.45 s head start and reads whole files (~70 MB at ~50 MB/s), so
  exec → `main()` is still 0.94 s at boot vs 0.05 s warm. Next lever: see
  below.
- `ultima-app/qml/Camera360Screen.qml`: SurroundView behind a `Loader`,
  armed 4 s after startup or on first open, whichever first.
- `tisdk-base-image.bbappend` `ultima_beagley_mask_units`: rpcbind,
  nfs-statd, remote-fs, avahi, iptables/ip6tables, docker.socket,
  gplv3-notice, networkd-wait-online, resolved.
- `main.cpp`: two more startup marks (`core objects created`, `QML engine
  ready`) — both ~0.02 s, i.e. the QML load line is now purely
  `engine.load()`.

**Steady-state profile now (warm reboot, kernel_ts):** init 0.85 → systemd
unit load done 1.70 → first jobs 1.96 → app exec 2.41 (gated on
`ultima-data-mount` 0.37 s and the splash 0.32 s, both finishing ~2.31) →
`main()` 3.35 → QGuiApplication 3.78 → QML loaded 5.04 → first frame 5.17.
Coldplug 5.21–6.13, Ethernet link 10.8 s, SSH ~22 s after power-on.

**Not done / next levers, in order of expected payoff:** (1) a recorded
page-range readahead (mincore at first frame → replay with `readahead(2)`)
in place of the whole-file prefetch — the app touches a fraction of the
70 MB, and it's the 0.9 s exec→`main()` plus part of the QML load; (2)
`/data` as a plain `data.mount` unit instead of the shell script, and not
gating the app on the splash — together ~0.3 s off the app's start; (3)
`systemd-analyze` in the image to see the 0.62 s of unit loading properly;
(4) QML load itself (1.26 s warm) — instantiation of ~2900 lines of QML plus
image decode, unprofiled; (5) UHS/SDR104 in the R5 SPL (it runs the card at
HS ~20 MB/s; the kernel gets SDR104) for another ~0.6 s off the FIT read —
needs `SPL_MMC_UHS_SUPPORT`/`SPL_MMC_IO_VOLTAGE` and the `vdd_sd_dv`
regulator in the R5 DT, and a bad SPL means a card swap; (6) the A53s run
at 1.2 GHz with no OPP table in this DT.

**Bench notes from this pass:** with the wic reflashed the boot partition is
just `tiboot3.bin` + `tifalcon.bin` (22 MB used of 128) — the pre-falcon
stock files are gone from the card, recovery is `flash.sh` or a FIT copy
from the Mac. `flash.sh` works unattended here (passwordless sudo). And the
mask-udev-trigger experiment is not repeatable without a card swap: with no
coldplug, systemd never sees `eth0` or `ttyS2` — no SSH, no getty — and the
ext4 root can't be edited from macOS; it was recovered with a falcon FIT
carrying `init=/bin/sh` in its baked bootargs and the serial console
scripted from the Mac (`boot-logs/beagley-ai-rescue-20260830T114107.txt`).
The serial adapter's TX works; bash's prompt there is `sh-5.2# `, not `# `.
CAN: no `can0` exists on this board's DT yet (the MCAN nodes need an
overlay) — nothing here changed that, it just isn't a regression.

### BeagleY-AI boot-time work, cycle 2 (2026-08-30): 6.9 s → 5.3 s cold

Same method as above (timed serial captures, the app's stderr marks,
`QSG_RENDER_TIMING`, `strace -f -tt` on the app, restart-with-dropped-caches
A/Bs), same constraints (GPU on, nothing the dash needs removed).

| power-on → ultima-app first frame     | cold  | warm `reboot` | kernel-relative |
|---------------------------------------|------:|--------------:|----------------:|
| end of cycle 1                        | 6.94  | 7.53          | 5.17            |
| + OP-TEE hwrng (CRNG ready at 0.24 s) |   —   | —             | 4.56            |
| + 6 systemd generators removed        |   —   | —             | 4.38            |
| **+ app-side work, final image**      | **5.31** (5.62 on the image's first boot, empty caches) | **6.27** (SPL banner → frame 5.30) | **3.86** (4.19 first boot) |

Cold power-on, SPL banner at +0.005 s: SPL → ATF 1.23 s, ATF → kernel 0
≈ 0.2 s, kernel → init 0.85 s, init → app exec 1.46 s, exec → first frame
1.55 s. The day started at 9.26 s; 43 % of it is gone with nothing the dash
needs removed. Log: `boot-logs/beagley-ai-falcon-cycle2-final-coldboot2-timed-20260830T140432.txt`.

The app's own `main()` → first frame, restart on the booted board with
`echo 3 > drop_caches` (so: the app, not the boot):

| build                                             | `main()` → first frame | render thread's first frame |
|---------------------------------------------------|-----------------------:|----------------------------:|
| end of cycle 1                                    | 1.74–1.90 s            | 565 ms (sync 70, render 485) |
| CameraView: no FBO node while hidden              | 1.33–1.45 s            | 163 ms (sync 54, render 105) |
| Window shown from C++, splash decoded off-thread  | 1.21–1.33 s (true first frame; the earlier marks were the *second* frame, ~120 ms late) | 190 ms |
| splash pre-decoded from `main()` (image provider) | 1.11–1.26 s            | 190 ms (sync 66, render 111) |

At boot (warm reboot, kernel_ts): app exec 2.27 → `main()` 2.37 →
QGuiApplication 2.82 → QML loaded 3.43 → window shown 3.48 → first frame
3.87. Logs: `boot-logs/beagley-ai-falcon-cycle2-final-coldboot-timed-20260830T130506.txt`
(first boot of the flashed image), `...-final-warm-timed-*.txt`.

**Root causes found this cycle, in the order they were found:**

1. **`getrandom()` blocked the app until the CRNG was ready.** `main()` sat
   at kernel_ts ≈ 3.4 s on every boot no matter how early the unit was
   exec'd or how much was prefetched — a `QT_HASH_SEED=0` experiment just
   moved the same wait into QGuiApplication (Mesa/PVR ask for randomness
   too). dmesg: `random: crng init done` at 3.4 s after a 0.9 s silent gap.
   The SoC's TRNG lives behind OP-TEE (SA2UL is firewalled to secure world)
   and is reachable as a hwrng through `optee_rng`; as modules, TEE +
   optee_rng only loaded at udev coldplug, i.e. after the app. Built in
   (`CONFIG_TEE/OPTEE/HW_RANDOM_OPTEE=y` in `ultima-boot.cfg`): `crng init
   done` at **0.236 s**, `main()` 3.35 → 2.43 s, first frame 5.17 → 4.56.
2. **Whole-file prefetch → recorded page ranges.** `recipes-ultima/
   ultima-readahead` (`record <pack> <pid>` = mmap + mincore over
   `/proc/<pid>/maps` in map order, gaps ≤ 64 KiB bridged; `replay <pack>`
   = `readahead(2)` per range, a file whose size changed is read whole
   instead, ≤ 32 MiB). `ultima-app/beagley-ai/readahead.pack`: 59 files,
   42 MB of ranges; `ultima-prefetch` uses it when present and falls back to
   the `cat` list. Exec → `main()` at boot 0.94 → 0.10 s. Record it on a
   boot with `ultima-prefetch` masked, right after the first frame, or the
   cache lies (instructions in the `ultima-prefetch` script).
3. **Unit plumbing in front of the app.** `RuntimeDirectory=ultima`
   instead of two `ExecStartPre=mkdir` forks (0.15 s between "Starting" and
   the app's exec); the board's own `ultima-splash.service` without
   `Before=ultima-app.service` and its own `ultima-data-mount.service`
   (plain oneshot `mount`, not a `data.mount` unit — those wait for the
   device unit, i.e. for udev). App exec at boot 2.41 → 2.26 s.
4. **systemd generators.** Nine of them fork+exec in series before the first
   job is queued; six do nothing here (gpt-auto, hibernate-resume,
   system-update, rc-local, debug, run) — removed at rootfs postprocess
   (`ultima_beagley_drop_generators`). "Hostname set" → "Queued start job"
   0.53 → 0.455 s.
5. **Five hidden `CameraView`s built their FBOs in the first frame.**
   `QSG_RENDER_TIMING` showed the render thread's first frame at 565 ms of
   which the scene-graph renderer was 105; `strace` put the rest in a
   400 ms window with no syscalls. It was `QQuickFramebufferObject`: Qt
   creates the renderer and FBO in `updatePaintNode()` even for a hidden
   item and runs its `render()` from `beforeRendering` — so the four
   CameraGridScreen views and the RearCameraScreen one each compiled
   blit + mirror GLSL (ten PowerVR compiles, ~40 ms each) and allocated
   an FBO, for screens nobody could see. `CameraView::updatePaintNode` now
   returns nothing while hidden and node-less (a visibility change dirties
   the item, so the node is built on first show); `ShaderManager` uses
   `addCacheableShaderFromSourceCode` so even that first show hits the
   program-binary cache after the first run. First frame 565 → 163 ms;
   camera grid first open 42 ms, second 12 ms.
6. **The window was shown before the first-frame hooks existed, and the
   splash PNGs decoded on the GUI thread.** `main.qml`'s Window is now
   `visible: false` and `main.cpp` shows it after connecting
   afterRendering/frameSwapped — so READY=1 and the marks are the real
   first frame (they used to be the second, 120 ms later). The two intro
   bitmaps are decoded from the start of `main()` on their own threads
   (`SplashImageProvider`, `image://splash/screen|car`) — Qt's asynchronous
   Image path is one reader thread and the pair took ~0.6 s there, longer
   than the rest of the QML took to instantiate — and `background_overlay`
   / `car_360` (both 1600x720, both invisible on the first frame) are
   `asynchronous: true`. The window is shown when `introAssetsReady`, or
   after 1.5 s regardless. The intro fade is gated on `root.visible` since
   the scene graph's animation timer ticks with no window exposed.

**Tried, no gain:** a throwaway `QOpenGLContext` on a helper thread right
after QGuiApplication to pre-warm PowerVR — context creation is 24 ms, the
cost was item 5. `QT_HASH_SEED=0` — see item 1.

**Not done / next levers:** (1) the A53s run at **1.2 GHz**: no OPP table
and no `ti,j722s` match in `ti-cpufreq` in this 6.12 tree (the sibling
AM62P5 has both, up to 1.25 GHz), U-Boot's J722S EVM R5 DT asks for
1.4 GHz. The board idles at 71–73 °C with the app up and there is no
cpufreq cooling device, so raising the clock is a thermal question first —
not just `assigned-clock-rates`. (2) QGuiApplication's 0.4 s: ~190 ms of
it is a syscall-free stretch right after `drmModeGetConnector`, i.e. PVR's
`eglInitialize`; single-threaded, unavoidable short of a lighter platform
plugin. (3) First-frame sync 66 ms + instantiation 0.4 s — the five screens
are all instantiated eagerly; `Loader`s would cut both. (4) systemd unit
load 0.45 s (218 units) and the 0.22 s to exec systemd itself off a cold
card. (5) UHS in the R5 SPL — **tried 2026-08-30, dead end, see below.**

### UHS/SDR104 in the R5 SPL: dead end (2026-08-30)

The biggest remaining lever looked like the SPL→ATF gap: the R5 SPL streams
the ~22 MiB `tifalcon.bin` off the SD FAT partition in ~1.2 s (≈18 MB/s =
the high-speed 4-bit ceiling), while the *kernel* runs this same card at
SDR104 (~4× the clock). So: teach the R5 SPL to do the 1.8 V UHS switch.
It doesn't work on this board, for an architectural reason worth recording
so nobody burns another card swap on it.

The DT is fully ready — `&sdhci1` has `vqmmc-supply = <&vdd_sd_dv>` (a
`regulator-gpio` switching 3.3↔1.8 V on `main_gpio1[49]`), `vmmc-supply =
<&vdd_mmc1>` (a fixed reg gpio-enabled on `main_gpio1[50]`), the
`ti,otap-del-sel-sdr104` tap delay, and `bootph-all` on the mmc node, both
regulators, `main_gpio1`, and the voltage-switch pinmux. The config side
(all confirmed landing in the generated `.config`): `MMC_UHS_SUPPORT` +
`MMC_IO_VOLTAGE` + `DM_REGULATOR{,_GPIO,_FIXED}` + `GPIO`/`DM_GPIO` +
`DA8XX_GPIO` (the driver for `ti,am64-gpio`/`ti,keystone-gpio`), each with
its `SPL_` twin. SPL grew 317 → 324 KiB, no size overflow.

Result on hardware: the SPL **can't drive `main_gpio1` at all**, so the
`vdd_sd_dv` regulator can't switch, so it fails to set vqmmc to *either*
voltage, and — because `MMC_IO_VOLTAGE` makes the SDHCI driver require the
vqmmc set even for the default 3.3 V — SD init fails outright and the SPL
halts at `### ERROR ### Please RESET the board ###` (a non-booting board,
recovered by restoring the prior `tiboot3.bin` to the FAT partition; the
card content is untouched, the ROM power-cycles at 3.3 V on the next boot).
Not graceful HS fallback — worse than the stock SPL.

Root cause, from the serial log: `ti_power_domain_of_xlate: invalid
dev-id: 78`. Dev-id 78 is `main_gpio1` (`power-domains = <&k3_pds 78>`).
`lpsc_lookup()` in `drivers/power/domain/ti-power-domain.c` rejects it —
main_gpio1 is not in the R5 SPL's compiled SoC power-domain (PSC/LPSC)
tables, so the GPIO can't be powered on, so the gpio-regulator can't
toggle. The boot-essential devices (sdhci1 itself, etc.) are in those
tables; a MAIN-domain GPIO used only for a vqmmc switch is not. Fixing it
means hand-patching TI's generated per-SoC power-domain data to add dev 78
with its correct PSC mapping — fragile, on the boot-critical path — and
even then the cold-start tune could still be marginal. This is exactly why
neither the stock `am67a_beagley_ai_r5_defconfig` nor TI's
`j722s_evm_r5_defconfig` enables SPL UHS: it's not a config flip, it needs
SoC-data work TI didn't do. Reverted; ~0.8 s not worth patching generated
PSC tables under a car dash's must-boot constraint. If ever revisited, the
one required change is main_gpio1 (dev 78) in the R5 power-domain table —
start there, not at the MMC config.

**Bench notes:** `strace` under a `Type=notify` unit makes strace the main
PID, READY=1 is ignored, and the unit times out after 45 s — run the
experiment with `NotifyAccess=all` or expect the restart. Drop-ins in
`/run/systemd/system/<unit>.d/` work on the read-only root and vanish at
reboot — the right tool for env-var experiments (`QSG_RENDER_TIMING=1`,
`QML_IMPORT_TRACE=1`). The app's `/tmp/ultima-screenshot.request` and
`/tmp/ultima-camtest.request` triggers were what made the camera-grid check
and the "is the first frame really the splash" check possible over SSH.
