# Ultima RPi5 — Complete Setup & Reproduction Guide

A comprehensive guide for reproducing the Ultima project: a minimal Buildroot-based Linux image for Raspberry Pi 5 running a fullscreen gauge cluster Qt5 app with fast boot, WiFi, and touchscreen support.

---

## Table of Contents

1. [Overview](#overview)
2. [Hardware Requirements](#hardware-requirements)
3. [Host Machine Setup (macOS)](#host-machine-setup-macos)
4. [Build VM Setup (Ubuntu)](#build-vm-setup-ubuntu)
5. [Project Structure](#project-structure)
6. [Buildroot Configuration](#buildroot-configuration)
7. [Kernel Configuration](#kernel-configuration)
8. [Boot Configuration](#boot-configuration)
9. [Init System & Boot Optimization](#init-system--boot-optimization)
10. [WiFi & Networking](#wifi--networking)
11. [Qt5 Application](#qt5-application)
12. [CAN Bus Integration (Syvecs S7+)](#can-bus-integration-syvecs-s7)
13. [Display Configuration](#display-configuration)
14. [Build Process](#build-process)
15. [Flashing](#flashing)
16. [RPi5 EEPROM Configuration](#rpi5-eeprom-configuration)
17. [SSH Access](#ssh-access)
18. [Debugging](#debugging)
19. [Known Issues & Workarounds](#known-issues--workarounds)

---

## Overview

**What this is**: A minimal Linux image (~320MB) that boots a Raspberry Pi 5 directly into a fullscreen Qt5 QML gauge cluster app. No desktop environment, no login prompt — power on and the app appears.

**Key design decisions**:
- BusyBox init (not systemd) for fast boot
- Qt5 EGLFS backend (direct to DRM/KMS, no X11/Wayland)
- Boot-optimized init ordering: app launches before networking/SSH
- WiFi connects in background after app is displayed
- VC4 display driver built into kernel; brcmfmac WiFi as module
- Read-only root filesystem (protects against SD card corruption from power cuts)
- Separate writable `/data` partition for persistent odometer state
- Image-based gauge rendering: background PNG + rotated needle PNG overlay
- Dashboard warning indicators (ISO 7000 icons), boost gauge, gear indicator, odometer/trip odo with persistence

**Boot timing** (power to app visible): ~9.2s
- Bootloader: ~7.8s (fixed hardware init — SDRAM, RP1 firmware, PCI)
- Kernel + init → app launch: ~0.77s
- App init (Qt/EGL/QML): ~0.68s

**Boot sequence**:
```
RPi5 Bootloader → Kernel (quiet) → S00remountro (root → ro, mount /data & /boot, launch Qt app)
                                      → S10udev
                                        → S11app (stub — app already running)
                                          → S20* deferred services
                                            → S40network (WiFi, backgrounded)
                                              → S50dropbear (SSH)
```

All init script output is silenced (redirected to `/dev/null` in rcS) to avoid serial console overhead.

---

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| **Board** | Raspberry Pi 5 — 8GB |
| **Display** | Waveshare 10.4" QLED 1600x720 via HDMI + USB touch |
| **Boot media** | microSD card (recommended) or USB flash drive |
| **WiFi** | Built-in BCM43455 (on-board) |
| **Power** | USB-C PD, 5V/5A recommended |

**Display notes**: The Waveshare connects via HDMI for video and USB for touch input. Qt KMS config targets `HDMI1` at `1600x720`.

---

## Host Machine Setup (macOS)

### Prerequisites
- [OrbStack](https://orbstack.dev/) — lightweight VM manager (or any Ubuntu VM with SSH)
- [ImageMagick](https://imagemagick.org/) — `brew install imagemagick` (for splash screen generation)
- Standard dev tools: `git`, `rsync`

### Directory Layout (on Mac)
```
~/code/ultima/                    # Project root (this repo)
├── br2-external/                 # Buildroot external tree
├── scripts/                      # Build/flash/debug scripts
├── output/                       # Built images (gitignored)
└── build/                        # Local dev builds (gitignored)
```

---

## Build VM Setup (Ubuntu)

The build runs inside an Ubuntu VM because Buildroot cross-compilation requires a Linux host.

### VM Details
- **VM name**: `ubuntu` (OrbStack)
- **SSH access**: `ssh ubuntu@orb` (user: `jellis`)
- **Buildroot location**: `~/ultima/buildroot/`
- **Project files synced to**: `~/ultima/`

### Initial VM Setup

Run `scripts/setup-vm.sh` on the VM, or manually:

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential libncurses-dev unzip rsync bc git wget \
    cpio python3 file libssl-dev python3-setuptools perl \
    patch gzip bzip2 xz-utils tar device-tree-compiler mtools

# Install git-lfs (needed if doing EEPROM updates)
sudo apt-get install -y git-lfs

# Clone Buildroot 2025.02
cd ~/ultima
git clone --depth 1 --branch 2025.02 \
    https://gitlab.com/buildroot.org/buildroot.git buildroot
```

### GCC Version Fix
Ubuntu 25.10+ ships GCC 15, which breaks Buildroot's host-m4 1.4.19 gnulib. You **must** install and use GCC 14:

```bash
sudo apt-get install -y gcc-14 g++-14
```

All `make` commands must include: `HOSTCC=gcc-14 HOSTCXX=g++-14`

### qt5declarative Patch
GCC 14+ requires explicit `#include <cstdint>` in qt5declarative. Create this patch at:
`~/ultima/buildroot/package/qt5/qt5declarative/0003-qv4compiler-add-missing-cstdint-include.patch`

(Check if this patch already exists in Buildroot 2025.02 — it may have been upstreamed.)

---

## Project Structure

```
br2-external/
├── Config.in                          # Sources ultima-app package config
├── external.desc                      # name: ULTIMA
├── external.mk                       # Includes all package makefiles
├── configs/
│   └── ultima_rpi5_defconfig          # Buildroot defconfig
├── board/ultima-rpi5/
│   ├── config.txt                     # RPi5 boot config
│   ├── cmdline.txt                    # Kernel command line
│   ├── kernel-fragments.cfg           # Kernel config fragments
│   ├── disable-usb0.dts               # DT overlay to disable unused USB controller
│   ├── genimage.cfg                   # Static genimage config (overridden by post-image.sh)
│   ├── post-build.sh                  # Post-build: disable getty, reorder init scripts, compile DT overlays
│   ├── post-image.sh                  # Post-image: creates data partition, dynamic genimage, EEPROM
│   └── overlay/
│       ├── boot/                      # Mountpoint for boot partition
│       ├── data/                      # Mountpoint for persistent data partition
│       ├── etc/
│       │   ├── fstab                  # Mount table (adds /var tmpfs for ro root)
│       │   ├── inittab                # Silenced inittab (no swapon, no getty)
│       │   ├── init.d/
│       │   │   ├── rcS                # Init runner — silences all script output (> /dev/null)
│       │   │   ├── S00remountro       # Remount root ro, mount partitions, launch app (before udev)
│       │   │   ├── S11app             # Stub: stop/restart only (app launched by S00remountro)
│       │   │   └── S40network         # WiFi (backgrounded)
│       │   ├── network/
│       │   │   └── interfaces         # Network interface config
│       │   ├── wpa_supplicant/
│       │   │   └── wpa_supplicant.conf
│       │   ├── udev/rules.d/
│       │   │   ├── 99-input.rules     # Touchscreen input device rules
│       │   │   └── 70-can.rules       # Auto-configures can0 (bitrate + up) on CAN adapter plug-in
│       │   └── qt-kms.conf            # Qt KMS display config
│       ├── lib/firmware/brcm/
│       │   ├── brcmfmac43455-sdio.bin
│       │   ├── brcmfmac43455-sdio.clm_blob
│       │   ├── brcmfmac43455-sdio.txt
│       │   └── brcmfmac43455-sdio.raspberrypi,5-model-b.txt
│       └── root/
│           └── .ssh/authorized_keys   # SSH public key
└── package/ultima-app/
    ├── Config.in
    ├── ultima-app.mk
    ├── can/
    │   └── syvecs_s7_fixed_stream_v3.dbc  # Reference DBC (CAN1 fixed stream — see CAN Bus Integration)
    └── src/
        ├── ultima-app.pro
        ├── main.cpp                   # App entry point + OdoStore/CanBus setup + SIGTERM handler
        ├── odostore.h                 # OdoStore class header (persistent odometer state)
        ├── odostore.cpp               # OdoStore implementation (reads/writes /data/odometer.json)
        ├── canbus.h                   # CanBus class header (live CAN2 data source, exposed as `sim`)
        ├── canbus.cpp                 # CanBus implementation: SocketCAN reader + frame decoder + macOS sim fallback
        ├── main.qml                   # Root layout: gauges, indicators, gear, odo, save timer
        ├── CircularGauge.qml          # Reusable needle gauge (rotates needle.png)
        ├── SimEngine.qml              # Unused at runtime — reference/potential --demo fallback (see below)
        ├── qml.qrc                    # Qt resource file
        ├── background.png             # 1600x720 gauge cluster background
        ├── needle.png                 # Gauge needle image
        ├── left_indicator.png         # Turn signal icon
        ├── boost_lines_overlay.png     # Boost gauge scale lines overlay
        ├── range.regular.ttf          # Font for odometer and boost PSI display
        ├── bahnschrift._semibold.ttf  # Font for gear indicator
        ├── icon_low_beam.png          # ISO 7000 low beam indicator
        ├── icon_high_beam.png         # ISO 7000 high beam indicator
        ├── icon_oil_pressure.png      # ISO 7000 oil pressure warning
        ├── icon_check_engine.png      # ISO 7000 check engine warning
        ├── icon_battery.png           # ISO 7000 battery warning
        └── icon_coolant_warn.png      # ISO 7000 coolant temp warning

scripts/
├── setup-vm.sh      # VM dependency installation
├── build.sh         # Full sync + build + copy image
├── flash.sh         # macOS SD card flasher with disk picker
├── dev-build.sh     # Local macOS Qt6 dev build
└── read-logs.sh     # Read debug logs from boot partition
```

---

## Buildroot Configuration

**Defconfig**: `br2-external/configs/ultima_rpi5_defconfig`

### Key Buildroot Settings

| Setting | Value | Why |
|---------|-------|-----|
| `BR2_aarch64=y` / `BR2_cortex_a76=y` | RPi5 CPU | |
| `BR2_KERNEL_HEADERS_6_6=y` | Must set explicitly | Custom tarball version detection fails, breaking glibc |
| `BR2_TOOLCHAIN_BUILDROOT_WCHAR=y` | wchar support | Required for Qt5/libinput/eudev |
| `BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV=y` | eudev | Creates `/dev/input` nodes for touchscreen |
| `BR2_INIT_BUSYBOX=y` | BusyBox init | Fast boot (not systemd) |
| `BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION` | RPi Linux 6.6.y | Raspberry Pi kernel fork |
| `BR2_LINUX_KERNEL_DEFCONFIG="bcm2712"` | RPi5 SoC | |
| `BR2_PACKAGE_RPI_FIRMWARE_VARIANT_PI4_64` | Firmware variant | Covers RPi5 on Buildroot 2025.02 |
| `BR2_TARGET_ROOTFS_EXT2_SIZE="320M"` | Root filesystem size | |
| `BR2_PACKAGE_QT5BASE_PNG=y` | PNG image support | Required for background and needle images |
| `BR2_PACKAGE_QT5BASE_DEFAULT_QPA="eglfs"` | Direct rendering | No X11/Wayland needed |
| `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_V3D=y` | GPU compute | |
| `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VC4=y` | HDMI output | |
| `BR2_PACKAGE_DHCPCD=y` | DHCP client | Redundant with udhcpc but kept in defconfig |
| `BR2_PACKAGE_DROPBEAR=y` | SSH server | Lightweight SSH |
| `BR2_SYSTEM_DHCP="wlan0"` | DHCP interface | |

### Full defconfig
```
# Architecture
BR2_aarch64=y
BR2_cortex_a76=y

# Kernel headers (must come before toolchain C library selection)
BR2_KERNEL_HEADERS_6_6=y

# Toolchain
BR2_TOOLCHAIN_BUILDROOT_GLIBC=y
BR2_TOOLCHAIN_BUILDROOT_CXX=y
BR2_TOOLCHAIN_BUILDROOT_WCHAR=y

# Device management - eudev for /dev/input nodes
BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV=y

# Init system - BusyBox for fast boot
BR2_INIT_BUSYBOX=y

# Kernel
BR2_LINUX_KERNEL=y
BR2_LINUX_KERNEL_CUSTOM_TARBALL=y
BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION="https://github.com/raspberrypi/linux/archive/rpi-6.6.y.tar.gz"
BR2_LINUX_KERNEL_DEFCONFIG="bcm2712"
BR2_LINUX_KERNEL_DTS_SUPPORT=y
BR2_LINUX_KERNEL_INTREE_DTS_NAME="broadcom/bcm2712-rpi-5-b"
BR2_LINUX_KERNEL_NEEDS_HOST_OPENSSL=y

# Kernel modules needed
BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_ULTIMA_PATH)/board/ultima-rpi5/kernel-fragments.cfg"

# Bootloader / firmware
BR2_PACKAGE_RPI_FIRMWARE=y
BR2_PACKAGE_RPI_FIRMWARE_VARIANT_PI4_64=y

# Filesystem
BR2_TARGET_ROOTFS_EXT2=y
BR2_TARGET_ROOTFS_EXT2_4=y
BR2_TARGET_ROOTFS_EXT2_SIZE="320M"
BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL_ULTIMA_PATH)/board/ultima-rpi5/overlay"
BR2_ROOTFS_POST_BUILD_SCRIPT="$(BR2_EXTERNAL_ULTIMA_PATH)/board/ultima-rpi5/post-build.sh"
BR2_ROOTFS_POST_IMAGE_SCRIPT="$(BR2_EXTERNAL_ULTIMA_PATH)/board/ultima-rpi5/post-image.sh"

# Image generation
BR2_PACKAGE_HOST_GENIMAGE=y
BR2_PACKAGE_HOST_DOSFSTOOLS=y
BR2_PACKAGE_HOST_MTOOLS=y

# Qt5
BR2_PACKAGE_QT5=y
BR2_PACKAGE_QT5BASE=y
BR2_PACKAGE_QT5BASE_OPENGL_LIB=y
BR2_PACKAGE_QT5BASE_PNG=y
BR2_PACKAGE_QT5BASE_EGLFS=y
BR2_PACKAGE_QT5BASE_FONTCONFIG=y
BR2_PACKAGE_QT5BASE_DEFAULT_QPA="eglfs"
BR2_PACKAGE_QT5DECLARATIVE=y
BR2_PACKAGE_QT5DECLARATIVE_QUICK=y

# Graphics - Mesa for V3D (RPi5)
BR2_PACKAGE_MESA3D=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_V3D=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VC4=y
BR2_PACKAGE_MESA3D_OPENGL_EGL=y
BR2_PACKAGE_MESA3D_OPENGL_ES=y
BR2_PACKAGE_LIBDRM=y

# Input - touch support
BR2_PACKAGE_LIBINPUT=y
BR2_PACKAGE_EVTEST=y

# WiFi
BR2_PACKAGE_WPA_SUPPLICANT=y
BR2_PACKAGE_WPA_SUPPLICANT_NL80211=y
BR2_PACKAGE_WPA_SUPPLICANT_AUTOSCAN=y
BR2_PACKAGE_LINUX_FIRMWARE=y
BR2_PACKAGE_LINUX_FIRMWARE_BRCM_BCM43XX=y

# Networking
BR2_PACKAGE_DHCPCD=y
BR2_PACKAGE_IPROUTE2=y

# Fonts
BR2_PACKAGE_DEJAVU=y

# Utilities
BR2_PACKAGE_NANO=y
BR2_PACKAGE_DROPBEAR=y

# System
BR2_TARGET_GENERIC_HOSTNAME="ultima"
BR2_TARGET_GENERIC_ISSUE="Ultima RPi5"
BR2_TARGET_GENERIC_GETTY_PORT="ttyAMA10"
BR2_TARGET_GENERIC_GETTY_BAUDRATE_115200=y
BR2_SYSTEM_DHCP="wlan0"

# Ultima app package
BR2_PACKAGE_ULTIMA_APP=y

# Strip for size
BR2_STRIP_strip=y
BR2_OPTIMIZE_S=y
BR2_ENABLE_LOCALE_PURGE=y
```

---

## Kernel Configuration

**File**: `br2-external/board/ultima-rpi5/kernel-fragments.cfg`

These fragments override the bcm2712 defconfig. The full file includes display/WiFi requirements plus extensive boot time optimizations:

**Display & WiFi (required)**:
```
CONFIG_MODULE_COMPRESS_NONE=y          # BusyBox modprobe can't handle .ko.xz
CONFIG_SOUND=y / CONFIG_SND=y / CONFIG_SND_SOC=y  # VC4 depends on SND && SND_SOC
CONFIG_DRM_VC4=y / CONFIG_DRM_V3D=y    # Display driver (built-in)
CONFIG_I2C=y / CONFIG_I2C_BRCMSTB=y    # I2C built-in for HDMI DDC (avoids ~1s deferred probe)
CONFIG_HID=y / CONFIG_USB_HID=y        # USB touchscreen
CONFIG_BRCMFMAC=m / CONFIG_BRCMUTIL=m  # WiFi as module (needs rootfs for firmware)
CONFIG_FAT_FS=y / CONFIG_VFAT_FS=y     # Mount boot partition for logs
```

**Boot optimizations (disabled unused features)**:
```
# CONFIG_KVM is not set              # No virtualization
# CONFIG_BT is not set               # Bluetooth disabled in config.txt
# CONFIG_NFS_FS is not set            # No network filesystems
# CONFIG_SCSI is not set              # No SCSI
# CONFIG_RC_CORE is not set           # No CEC/IR remote
# CONFIG_MEDIA_SUPPORT is not set     # No camera/video codec
# CONFIG_MACB is not set              # No wired ethernet
# CONFIG_FTRACE is not set            # No function tracing
# CONFIG_RANDOMIZE_BASE is not set    # No KASLR (avoids KPTI overhead)
# CONFIG_FB_SIMPLE is not set         # No simple-framebuffer (vc4 takes over)
# CONFIG_VGA_ARB is not set           # No VGA hardware
# CONFIG_USB_DWC2 is not set          # No OTG USB
# CONFIG_TASKSTATS is not set         # No taskstats/audit
# CONFIG_PROFILING is not set         # No profiling/perf
# CONFIG_DEBUG_FS is not set          # No debugfs
# CONFIG_KALLSYMS is not set          # No symbol tables
# CONFIG_IOSCHED_BFQ is not set       # Only mq-deadline scheduler
```

**Embedded optimizations**:
```
CONFIG_EMBEDDED=y                      # Enable embedded options
CONFIG_CC_OPTIMIZE_FOR_SIZE=y          # -Os (smaller kernel, ~net neutral on boot)
CONFIG_HZ_100=y / CONFIG_HZ=100       # Lower tick rate
CONFIG_SLAB_MERGE_DEFAULT=y            # Merge similar slabs
CONFIG_BASE_SMALL=1                    # Smaller hash tables
```

See `br2-external/board/ultima-rpi5/kernel-fragments.cfg` for the complete file.

### Critical Kernel Notes

- **VC4 depends on SND && SND_SOC** — without `CONFIG_SOUND=y`, `CONFIG_SND=y`, `CONFIG_SND_SOC=y`, the VC4 driver gets forced to `=m` and display won't work
- **I2C_BRCMSTB must be built-in** (`=y`) — the VC4 HDMI driver needs the DDC I2C adapter for EDID. If I2C is a module (default RPi defconfig), HDMI returns `-EPROBE_DEFER` causing ~1s of component framework retry cycles (18+ bind/unbind cycles)
- **DRM device ordering**: V3D = `/dev/dri/card0` (GPU compute only), VC4 = `/dev/dri/card1` (HDMI). Qt KMS config must target `card1`
- **Module compression**: `CONFIG_MODULE_COMPRESS_NONE=y` is mandatory — BusyBox modprobe can't decompress `.ko.xz` files
- **brcmfmac must be a module** (`=m`), not built-in (`=y`). Built-in loads before rootfs is mounted, so it can't find firmware files in `/lib/firmware/brcm/`

---

## Boot Configuration

### config.txt
**File**: `br2-external/board/ultima-rpi5/config.txt`

```
# RPi5 boot config
arm_64bit=1
kernel=Image

# GPU/Display - KMS driver (RPi5), disable unused HDMI1
dtoverlay=vc4-kms-v3d-pi5,nohdmi1

# Fast boot
disable_splash=1
boot_delay=0
force_turbo=1
dtparam=pciex1=off

# Disable Bluetooth
dtoverlay=disable-bt

# Disable unused USB controller (rp1_usb0) — touchscreen is on usb1
dtoverlay=disable-usb0

# Skip network install screen
disable_net_install=1

# Serial console
enable_uart=1
```

### cmdline.txt
**File**: `br2-external/board/ultima-rpi5/cmdline.txt`

**Production** (quiet boot — current setting):
```
root=/dev/mmcblk0p2 rootwait ro console=ttyAMA10,115200 loglevel=1 quiet logo.nologo vt.global_cursor_default=0 numa=off nokaslr
```

**Debug/profiling** (verbose UART console):
```
root=/dev/mmcblk0p2 rootwait ro console=ttyAMA10,115200 printk.time=1 loglevel=7 logo.nologo vt.global_cursor_default=0 numa=off nokaslr
```

**Important**: The verbose console adds ~1.7s to boot time. When the UART console is enabled with `loglevel=7`, the kernel flushes ~6KB of buffered early-boot messages over 115200 baud serial (~0.7s), and printk calls block throughout boot. Use `loglevel=1 quiet` for production — this keeps the UART console registered (for init script markers and SSH) but suppresses all but emergency kernel messages.

| Parameter | Purpose |
|-----------|---------|
| `root=/dev/mmcblk0p2` | SD card root. Change to `/dev/nvme0n1p2` for NVMe or `/dev/sda2` for USB |
| `rootwait` | Wait for root device to appear |
| `ro` | Mount root filesystem read-only (S00remountro ensures it stays ro) |
| `quiet` | Suppress kernel log messages during boot |
| `loglevel=1` | Only show emergency/alert messages (use `7` for debug) |
| `logo.nologo` | Disable Linux penguin logo |
| `console=ttyAMA10,115200` | UART console on JST-SH header (kept for APP_LAUNCH marker and SSH) |
| `vt.global_cursor_default=0` | Hide blinking cursor |
| `numa=off` | Disable NUMA — saves ~50ms on single-node SoC |
| `nokaslr` | Disable kernel address space randomization — avoids forced KPTI overhead |

---

## Init System & Boot Optimization

BusyBox init runs `/etc/init.d/S*` scripts in alphabetical order. The boot has been optimized so the app launches in **S00remountro** — before udev:

### Final Boot Order

| Script | Source | Purpose | Blocking? |
|--------|--------|---------|-----------|
| **S00remountro** | Custom overlay | Remount root ro, mount /data & /boot, **launch Qt app** | Yes |
| **S10udev** | Buildroot (eudev) | Device node creation | Yes |
| **S11app** | Custom overlay | Stub (app already running); handles stop/restart | No |
| S20seedrng | Buildroot (renamed from S01) | Seed RNG | Deferred |
| S20syslogd | Buildroot (renamed from S01) | System logger | Deferred |
| S20klogd | Buildroot (renamed from S02) | Kernel logger | Deferred |
| S20sysctl | Buildroot (renamed from S02) | Sysctl settings | Deferred |
| S20crond | Buildroot (renamed from S50) | Cron scheduler | Deferred |
| S40network | Custom overlay | WiFi (backgrounded) | No |
| S50dropbear | Buildroot | SSH server | Deferred |

### post-build.sh — Init Script Reordering

**File**: `br2-external/board/ultima-rpi5/post-build.sh`

This script runs during the Buildroot build and modifies the target rootfs:

```bash
#!/bin/bash
set -euo pipefail

BOARD_DIR="$(dirname "$0")"
TARGET_DIR="$1"

# Install boot config files
cp "$BOARD_DIR/config.txt" "$TARGET_DIR/../images/" 2>/dev/null || true
cp "$BOARD_DIR/cmdline.txt" "$TARGET_DIR/../images/" 2>/dev/null || true

# Ensure wpa_supplicant directory exists
mkdir -p "$TARGET_DIR/etc/wpa_supplicant"

# Ensure app directory exists
mkdir -p "$TARGET_DIR/root/app"

# Disable all gettys (no interactive consoles needed)
if [ -f "$TARGET_DIR/etc/inittab" ]; then
    sed -i 's|^tty1|#tty1|' "$TARGET_DIR/etc/inittab"
    sed -i 's|^ttyAMA10|#ttyAMA10|' "$TARGET_DIR/etc/inittab"
fi

# Defer non-essential init scripts to run AFTER S11app
INITD="$TARGET_DIR/etc/init.d"
for script in S01syslogd S01seedrng S02klogd S02sysctl S50crond; do
    if [ -f "$INITD/$script" ]; then
        newname="S20${script#S[0-9][0-9]}"
        mv "$INITD/$script" "$INITD/$newname"
    fi
done

# Remove S41dhcpcd — redundant with S40network's udhcpc
rm -f "$INITD/S41dhcpcd"

echo "Post-build script complete."
```

### S00remountro — Remount Read-Only + Launch App

**File**: `br2-external/board/ultima-rpi5/overlay/etc/init.d/S00remountro`

Runs as the very first init script (S00). Remounts root read-only, mounts /data and /boot partitions, and launches the Qt app immediately — **before udev (S10)**. This saves ~0.5s because the kernel has already created `/dev/dri/*` and `/dev/input/*` device nodes by this point.

```bash
#!/bin/sh
case "$1" in
    start)
        sync
        mount -o remount,ro /

        # Mount data partition for persistent odometer state
        mkdir -p /data
        mount -t ext4 /dev/mmcblk0p3 /data 2>/dev/null

        # Mount boot partition for logs
        mkdir -p /boot
        for dev in /dev/mmcblk0p1 /dev/sda1; do
            [ -b "$dev" ] && mount -t vfat "$dev" /boot 2>/dev/null && break
        done

        # Launch Qt app immediately (before udev)
        export QT_QPA_PLATFORM=eglfs
        export QT_QPA_EGLFS_KMS_CONFIG=/etc/qt-kms.conf
        export QT_QPA_EGLFS_INTEGRATION=eglfs_kms
        export QT_QPA_EGLFS_NO_LIBINPUT=1
        export XDG_RUNTIME_DIR=/tmp/runtime
        mkdir -p "$XDG_RUNTIME_DIR"

        # Timing marker on UART (visible even with quiet console)
        echo "[APP_LAUNCH] $(cut -d' ' -f1 /proc/uptime)" > /dev/ttyAMA10 2>/dev/null

        if mountpoint -q /boot 2>/dev/null; then
            /root/app/ultima-app > /boot/ultima-app.log 2>&1 &
        else
            /root/app/ultima-app > /var/log/ultima-app.log 2>&1 &
        fi
        ;;
    stop)
        killall -q ultima-app 2>/dev/null
        ;;
esac
```

**Why S00 and not in inittab?** BusyBox inittab remounts root `rw` during sysinit. Removing that line breaks the boot sequence (sysfs, devpts, etc. fail to mount). Instead, we let inittab run normally, then S00remountro flips root back to `ro` before any other init script runs.

**Why launch app before udev?** The vc4 DRM driver is built into the kernel (not a module), so `/dev/dri/card1` already exists when S00 runs. udev's coldplug re-scans devices but isn't needed for the app to start rendering.

Writable paths after remount:
- `/tmp`, `/run`, `/var` — tmpfs (from fstab)
- `/data` — separate ext4 partition (mounted here)
- `/dev` — devtmpfs

### fstab — Filesystem Mount Table

**File**: `br2-external/board/ultima-rpi5/overlay/etc/fstab`

Identical to Buildroot's default fstab plus `/var` as tmpfs (needed because root is read-only):

```
/dev/root   /         ext2    rw,noauto                             0 1
proc        /proc     proc    defaults                              0 0
devpts      /dev/pts  devpts  defaults,gid=5,mode=620,ptmxmode=0666 0 0
tmpfs       /dev/shm  tmpfs   mode=1777                             0 0
tmpfs       /tmp      tmpfs   mode=1777                             0 0
tmpfs       /run      tmpfs   mode=0755,nosuid,nodev                0 0
tmpfs       /var      tmpfs   mode=0755,nosuid,nodev                0 0
sysfs       /sys      sysfs   defaults                              0 0
```

### S11app — App Stub (Stop/Restart Only)

**File**: `br2-external/board/ultima-rpi5/overlay/etc/init.d/S11app`

The app is now launched by S00remountro. S11app is a stub that only handles `stop` and `restart`:

```bash
#!/bin/sh
#
# App is now launched by S00remountro (before udev) for fastest startup.
# This script only handles stop/restart.
#

case "$1" in
    start)
        # App already launched by S00remountro
        ;;
    stop)
        killall -q ultima-app 2>/dev/null
        ;;
    restart)
        "$0" stop
        sleep 1
        # Re-launch with proper env
        export QT_QPA_PLATFORM=eglfs
        export QT_QPA_EGLFS_KMS_CONFIG=/etc/qt-kms.conf
        export QT_QPA_EGLFS_INTEGRATION=eglfs_kms
        export QT_QPA_EGLFS_NO_LIBINPUT=1
        export XDG_RUNTIME_DIR=/tmp/runtime
        mkdir -p "$XDG_RUNTIME_DIR"
        if mountpoint -q /boot 2>/dev/null; then
            /root/app/ultima-app > /boot/ultima-app.log 2>&1 &
        else
            /root/app/ultima-app > /var/log/ultima-app.log 2>&1 &
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac
```

### S40network — WiFi (Backgrounded)

**File**: `br2-external/board/ultima-rpi5/overlay/etc/init.d/S40network`

```bash
#!/bin/sh
#
# Start WiFi networking (backgrounded for fast boot)
#

case "$1" in
    start)
        (
            LOGFILE="/boot/wifi-debug.log"
            echo "=== WiFi start $(cat /proc/uptime) ===" >> "$LOGFILE"
            modprobe brcmfmac >> "$LOGFILE" 2>&1
            echo "modprobe exit: $?" >> "$LOGFILE"
            # Wait for wlan0 to appear
            for i in $(seq 1 30); do
                [ -d /sys/class/net/wlan0 ] && break
                sleep 1
            done
            if [ -d /sys/class/net/wlan0 ]; then
                echo "wlan0 appeared after ${i}s" >> "$LOGFILE"
            else
                echo "ERROR: wlan0 never appeared after 30s" >> "$LOGFILE"
                ls /lib/modules/*/kernel/drivers/net/wireless/broadcom/brcm80211/ >> "$LOGFILE" 2>&1
                ls /lib/firmware/brcm/ >> "$LOGFILE" 2>&1
                dmesg | grep -i "brcm\|firm\|wlan\|wifi" >> "$LOGFILE" 2>&1
            fi
            wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf >> "$LOGFILE" 2>&1
            echo "wpa_supplicant exit: $?" >> "$LOGFILE"
            udhcpc -i wlan0 -b -q >> "$LOGFILE" 2>&1
            echo "udhcpc exit: $?" >> "$LOGFILE"
            ip addr show wlan0 >> "$LOGFILE" 2>&1
            echo "=== WiFi done $(cat /proc/uptime) ===" >> "$LOGFILE"
        ) &
        ;;
    stop)
        killall -q wpa_supplicant 2>/dev/null
        killall -q udhcpc 2>/dev/null
        ifconfig wlan0 down 2>/dev/null
        ;;
    restart)
        "$0" stop
        sleep 1
        "$0" start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac
```

---

## WiFi & Networking

### WiFi Firmware

Buildroot's `BRCM_BCM43XX` and `CYPRESS_CYW43XX` firmware packages don't install the correct files for RPi5's BCM43455. The firmware must be manually placed in the overlay:

**Location**: `br2-external/board/ultima-rpi5/overlay/lib/firmware/brcm/`

| File | Source |
|------|--------|
| `brcmfmac43455-sdio.bin` | Extract from a working Raspberry Pi OS installation |
| `brcmfmac43455-sdio.clm_blob` | Same source |
| `brcmfmac43455-sdio.txt` | NVRAM config — copy from RPi4 firmware |
| `brcmfmac43455-sdio.raspberrypi,5-model-b.txt` | RPi5-specific NVRAM (identical to above) |

### WPA Supplicant Config

**File**: `br2-external/board/ultima-rpi5/overlay/etc/wpa_supplicant/wpa_supplicant.conf`

```
country=US

network={
    ssid="YOUR_SSID"
    psk="YOUR_PASSWORD"
}
```

**Change the SSID and PSK to match your WiFi network.**

### Network Interfaces

**File**: `br2-external/board/ultima-rpi5/overlay/etc/network/interfaces`

```
auto lo
iface lo inet loopback

auto wlan0
iface wlan0 inet dhcp
    pre-up wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
    post-down killall -q wpa_supplicant || true
```

---

## Qt5 Application

### Package Definition

**File**: `br2-external/package/ultima-app/Config.in`
```
config BR2_PACKAGE_ULTIMA_APP
	bool "ultima-app"
	depends on BR2_PACKAGE_QT5BASE
	depends on BR2_PACKAGE_QT5DECLARATIVE
	help
	  Ultima fullscreen Qt5 QML application.
	  Fullscreen gauge cluster with persistent odometer.
```

**File**: `br2-external/package/ultima-app/ultima-app.mk`
```makefile
ULTIMA_APP_VERSION = 1.0
ULTIMA_APP_SITE = $(BR2_EXTERNAL_ULTIMA_PATH)/package/ultima-app/src
ULTIMA_APP_SITE_METHOD = local
ULTIMA_APP_DEPENDENCIES = qt5base qt5declarative
ULTIMA_APP_LICENSE = Proprietary

define ULTIMA_APP_CONFIGURE_CMDS
	cd $(@D) && $(QT5_QMAKE) $(ULTIMA_APP_SITE)/ultima-app.pro
endef

define ULTIMA_APP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)
endef

define ULTIMA_APP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ultima-app $(TARGET_DIR)/root/app/ultima-app
	$(INSTALL) -D -m 0644 $(ULTIMA_APP_SITE)/main.qml $(TARGET_DIR)/root/app/main.qml
endef

$(eval $(generic-package))
```

### Source Files

**`ultima-app.pro`**:
```pro
QT += qml quick
CONFIG += c++17
TARGET = ultima-app
HEADERS += odostore.h canbus.h
SOURCES += main.cpp odostore.cpp canbus.cpp
RESOURCES += qml.qrc
```

**`qml.qrc`**:
```xml
<RCC>
    <qresource prefix="/">
        <file>main.qml</file>
        <file>CircularGauge.qml</file>
        <file>SimEngine.qml</file>
        <file>background.png</file>
        <file>needle.png</file>
        <file>left_indicator.png</file>
        <file>range.regular.ttf</file>
        <file>bahnschrift._semibold.ttf</file>
        <file>icon_oil_pressure.png</file>
        <file>icon_check_engine.png</file>
        <file>icon_battery.png</file>
        <file>icon_coolant_warn.png</file>
        <file>boost_lines_overlay.png</file>
        <file>icon_low_beam.png</file>
        <file>icon_high_beam.png</file>
    </qresource>
</RCC>
```

**`main.cpp`** — creates `OdoStore` for persistent odometer and `CanBus` as the live gauge data source, exposes both to QML, saves odometer state on SIGTERM/SIGINT:
```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <signal.h>

#include "odostore.h"
#include "canbus.h"

static OdoStore *g_odoStore = nullptr;
static CanBus *g_canBus = nullptr;

static void sigHandler(int) {
    if (g_canBus)
        g_canBus->save();        // pushes latest odometer into OdoStore + persists
    else if (g_odoStore)
        g_odoStore->save();
    _exit(0);
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    OdoStore odoStore("/data/odometer.json");
    g_odoStore = &odoStore;

    CanBus canBus(&odoStore);
    g_canBus = &canBus;
    signal(SIGTERM, sigHandler);
    signal(SIGINT, sigHandler);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("odoStore", &odoStore);
    engine.rootContext()->setContextProperty("sim", &canBus);   // QML binds to `sim` — see CAN Bus Integration
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    return app.exec();
}
```

**`odostore.h` / `odostore.cpp`** — `OdoStore` is a `QObject` with `totalOdo` and `tripOdo` properties. Reads `/data/odometer.json` on construction (defaults to 2347.0 / 0.0 if missing). `save()` slot writes JSON. The 30s save timer in `main.qml` calls `sim.save()` periodically (routed through `CanBus::save()`, which flushes into `OdoStore` — see below), and the SIGTERM handler saves on `init stop`.

**`canbus.h` / `canbus.cpp`** — `CanBus` is the live gauge data source, exposed to QML as the `sim` context property (a drop-in replacement for `SimEngine.qml`'s property set, so `main.qml` didn't need to change). Full details — hardware, frame map, debugging — are in [CAN Bus Integration](#can-bus-integration-syvecs-s7) below. Summary: on Linux it opens a raw `PF_CAN` socket on `can0`, retrying every 1s until the interface exists (the app launches before udev), and decodes Syvecs S7+ CAN2 frames via a `QSocketNotifier`. On non-Linux (macOS dev builds), `#ifdef __linux__` compiles out all of the SocketCAN code and instead runs `simulateTick()` — a `QTimer`-driven simulator that reproduces `SimEngine.qml`'s phase-based driving profile (city/stop/suburban/highway/spirited legs) directly on `CanBus`'s member state, so the gauges animate without real CAN hardware. This is what backs the local macOS dev build (see [App Rebuild Shortcut](#app-rebuild-shortcut) and `scripts/dev-build.sh`).

### QML Files

The app consists of 3 QML files, all in `br2-external/package/ultima-app/src/`:

- **`main.qml`** — Root layout: 4 gauge needles over background image, boost gauge (trapezoid black overlay with PSI readout), turn signal indicators, top indicator row (oil, check engine, beams, battery, coolant — warnings flash at 300ms), gear indicator (Bahnschrift SemiBold font, P/R/N/1-7), odometer + trip odometer with reset button, touch feedback dot. Includes 30s periodic save timer for odometer persistence via `sim.save()`. All gauge values bind to the `sim` context property, which is the `CanBus` C++ object, not this file's `SimEngine.qml`
- **`CircularGauge.qml`** — Reusable needle gauge component: rotates `needle.png` over a transparent item positioned at the gauge center. Configurable start/end angles, counter-clockwise mode, needle size/pivot, optional debug arc overlay
- **`SimEngine.qml`** — **Not loaded at runtime.** Bundled only as a reference/potential `--demo` fallback; it predates `CanBus` and was the original simulated data source before real CAN integration. Speed wanders through city/suburban/highway/stop phases, RPM derived from gear ratios, automatic gear selection (P/R/N/1-7), fuel consumption, coolant temp, boost pressure (0-30 PSI). `CanBus::simulateTick()` (macOS builds only) reimplements this same phase logic directly in C++ so it can drive the live `sim` property — see [CAN Bus Integration](#can-bus-integration-syvecs-s7)

### Asset Files

| File | Purpose |
|------|---------|
| `background.png` | 1600x720 gauge cluster face (speedometer, tachometer, fuel, coolant, boost) |
| `needle.png` | Gauge needle image (rotated by CircularGauge) |
| `boost_lines_overlay.png` | Boost gauge scale lines overlay |
| `left_indicator.png` | Turn signal arrow icon (mirrored for right) |
| `range.regular.ttf` | Font for odometer and boost PSI display |
| `bahnschrift._semibold.ttf` | Bahnschrift SemiBold font for gear indicator |
| `icon_*.png` | ISO 7000 dashboard warning icons (51x51, white on transparent) |

### Gauge Needle Alignment

| Gauge | Pivot (px) | Start Angle | End Angle | Direction | Needle Size | Needle Pivot |
|-------|-----------|-------------|-----------|-----------|-------------|-------------|
| Speedometer | 351, 342 | 216.5 | 450.5 | CW | 98x350 | 48, 259 |
| Tachometer | 1251, 343 | 270 | 503 | CW | 98x350 | 48, 259 |
| Fuel level | 149, 602 | 217 | 307.5 | CW | 28x100 | 14, 74 |
| Coolant temp | 1453, 602 | 142 | 53.5 | CCW | 28x100 | 14, 74 |

### App Rebuild Shortcut

After changing only the Qt app source:
```bash
ssh ubuntu@orb "cd ~/ultima/buildroot && make ultima-app-dirclean && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"
```

After changing only overlay/init scripts:
```bash
ssh ubuntu@orb "cd ~/ultima/buildroot && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"
```

### Hot-Deploy to Pi (skip reflash)

Push just the binary to the Pi and restart without reflashing the SD card:

```bash
# Build on VM
ssh ubuntu@orb "cd ~/ultima/buildroot && make ultima-app-dirclean && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"

# Copy binary from VM
scp ubuntu@orb:~/ultima/buildroot/output/target/root/app/ultima-app /tmp/ultima-app

# Push to Pi and restart
scp -O /tmp/ultima-app root@192.168.50.139:/root/app/ultima-app
ssh root@192.168.50.139 "/etc/init.d/S11app restart"
```

**Important**: Deploy to `/root/app/ultima-app` — that's where the init scripts launch from.

### Local macOS Dev Build (with Simulated CAN Data)

For iterating on QML/layout without a Pi or CAN hardware, build and run the app natively on macOS with desktop Qt:

```bash
brew install qt   # Qt 6 via Homebrew — the .pro file has no Qt5-specific dependencies, builds fine under Qt6
scripts/dev-build.sh   # qmake6 + make in ./build/, then opens ultima-app.app
```

`CanBus` is written so the Linux-only `#ifdef __linux__` block (SocketCAN: `socket(PF_CAN, ...)`, `linux/can.h`, etc.) compiles out entirely on macOS. In its place, `CanBus::tryConnect()`'s `#else` branch starts a `QTimer`-driven `simulateTick()` that generates a realistic driving profile (see [CAN Bus Integration](#can-bus-integration-syvecs-s7) below), so the gauges animate immediately without any hardware. `main.cpp`'s `/proc/uptime` read and `OdoStore`'s `/data/odometer.json` path both fail open (silently) when missing on macOS, so nothing else needs stubbing.

---

## CAN Bus Integration (Syvecs S7+)

The dash gets live engine/vehicle data from the car's Syvecs S7+ ECU over CAN, decoded by `CanBus` (`canbus.h`/`canbus.cpp`) and exposed to QML as the `sim` context property.

### Hardware

- **Adapter**: ODrive USB-CAN adapter (`gs_usb`/candleLight class, VID:PID `1d50:606f`). Plugs into a Pi USB-A port; screw terminals wire to the ECU's **B2 (CAN_H) / B3 (CAN_L)** pins.
- **Termination**: the adapter has a switchable 120 Ω terminator built in. The Syvecs S7+'s CAN2 bus has **no on-board termination**, so an external 120 Ω resistor must be placed across CAN_H/CAN_L at the ECU end, or the bus won't terminate correctly.
- **Kernel support**: `CONFIG_CAN`, `CAN_RAW`, `CAN_BCM`, `CAN_DEV`, `CAN_GS_USB` are all `=y` in `kernel-fragments.cfg`.
- **Bring-up**: `br2-external/board/ultima-rpi5/overlay/etc/udev/rules.d/70-can.rules` matches the adapter's VID:PID and runs `ip link set can0 type can bitrate 1000000 && ip link set up can0` automatically on plug-in. Because the app launches before udev (see [Boot Optimizations](#boot-optimizations-applied)), `CanBus` doesn't assume `can0` exists at startup — it retries `socket(PF_CAN)` + `bind()` every 1s until the interface appears.

### ECU Configuration

The ECU is a Syvecs S7+ on a Cayman LS7 engine swap (firmware string `S7Plus 1.778.1- LS7SC - Cayman Standalone`), configured via Syvecs' SCal tool.

- **CAN1 is the powertrain bus — never touch it.** All dash data comes from **CAN2, running at 1 Mbit/s** (verified in SCal: Datastreams → CAN2 Bus Speed).
- CAN2's outbound frames are configured via SCal **Datastreams → Generic CAN Transmit**. This build does *not* have the "Custom CAN" / "Datastream Select" hierarchy that some gaugeART documentation references — don't assume that structure applies here.
- **Syvecs `.SC` config files are proprietary binary** (encrypted/packed after a small header) — there's no way to extract the CAN Tx config by inspecting an `.SC` file directly. If you need to verify or change the CAN2 layout, ask for a screenshot of the SCal Datastreams → Generic CAN Transmit screen rather than trying to parse a config file.

### DBC Reference (Scalings Only — Not Frame IDs)

`br2-external/package/ultima-app/can/syvecs_s7_fixed_stream_v3.dbc` is bundled as a reference, but it describes the **CAN1 fixed stream**, not this car's CAN2 layout. Its per-channel scaling and signedness definitions are tied to channel name (e.g. `vehicleSpeed`'s `0.036` km/h scaling), which holds true regardless of which frame/slot a channel is transmitted on — so it's useful for looking up scaling factors, but **do not use it to look up frame IDs** for CAN2.

### Verified CAN2 Frame Map

Read from SCal Datastreams → Generic CAN Transmit → Transmit Content. Frame *N* broadcasts on CAN ID `0x600 + (N-1)`; each frame carries four 16-bit big-endian slots, slot *S* at byte offset `2(S-1)..2S-1`. What `CanBus::decodeFrame()` consumes today:

| Frame (CAN ID) | Bytes | Channel | Decoding |
|---|---|---|---|
| `0x600` | 0-1 | rpm | signed, clamped ≥ 0 |
| `0x604` | 6-7 | limpMode | unsigned enum; non-zero → `checkEngine` |
| `0x605` | 2-3 | ect1 (coolant) | °C × 0.1 → °F; `coolantWarn` if > 110 °C |
| `0x608` | 0-1 | eop1 (oil pressure) | kPa × 0.1; `oilPressureWarn` if rpm > 1200 && < 100 kPa |
| `0x60E` | 2-3 | gear | Syvecs enum 0=Unknown 1=R 2=N 3..10=1st..8th → QML -1/0/1..8 |
| `0x60E` | 4-5 | vbat | V × 0.001 (unsigned); `batteryWarn` if 0.5 < v < 12 |
| `0x60F` | 0-1 | vehicleSpeed | km/h × 0.036 → mph (× 0.621371); drives odometer accumulation |
| `0x614` | 0-1 | mapMax (boost) | kPa abs × 0.1 → psi `(kPa-101.325)×0.145038`, clamped ≥ 0 (frame 21 — added separately from the initial SCal config) |

**Not on CAN2 in the current SCal config:**
- `flvlA` (fuel level) — the fuel gauge stays at 0 until this channel is added to SCal's Transmit Content.
- `sensorWarningLevel` — `checkEngine` currently derives from `limpMode` alone.

**Not on CAN at all** (no Syvecs channels exist for these): `leftIndicator`, `rightIndicator`, `lowBeams`, `highBeams` — declared as `CONSTANT`/`false` properties on `CanBus` purely so `main.qml`'s bindings still compile.

### Debugging

- `candump can0` (can-utils is enabled in the defconfig) to watch raw traffic.
- `ip -details link show can0` to check bitrate/link state.
- `CanBus` logs `[canbus] ...` lines to stderr on connect/bind failures and reconnects — check `/boot/ultima-app.log` or `/var/log/ultima-app.log` (see [Debugging](#debugging)).

---

## Display Configuration

### Qt KMS Config

**File**: `br2-external/board/ultima-rpi5/overlay/etc/qt-kms.conf`

```json
{
    "device": "/dev/dri/card1",
    "hwcursor": false,
    "outputs": [
        {
            "name": "HDMI1",
            "mode": "1600x720"
        }
    ]
}
```

**Critical**: Must be `card1`, not `card0`. On RPi5, V3D registers as `card0` (GPU compute only) and VC4 registers as `card1` (HDMI output).

### Qt Environment Variables (set by S00remountro)

```
QT_QPA_PLATFORM=eglfs
QT_QPA_EGLFS_KMS_CONFIG=/etc/qt-kms.conf
QT_QPA_EGLFS_INTEGRATION=eglfs_kms
QT_QPA_EGLFS_NO_LIBINPUT=1
XDG_RUNTIME_DIR=/tmp/runtime
```

`QT_QPA_EGLFS_NO_LIBINPUT=1` forces Qt to use evdev for touch input instead of libinput, which is more reliable with the Waveshare touchscreen.

---

## Build Process

### Quick Build (after initial setup)

From the Mac:

```bash
# 1. Sync project files to VM
rsync -av --exclude='buildroot/' --exclude='.git/' --exclude='output/' \
    ~/code/ultima/ ubuntu@orb:~/ultima/

# 2. Build on VM
ssh ubuntu@orb "cd ~/ultima/buildroot && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"

# 3. Copy image back
scp ubuntu@orb:~/ultima/buildroot/output/images/sdcard.img ~/code/ultima/output/sdcard.img
```

### First-Time Build

```bash
# 1. Sync files to VM
rsync -av --exclude='buildroot/' --exclude='.git/' --exclude='output/' \
    ~/code/ultima/ ubuntu@orb:~/ultima/

# 2. Load defconfig
ssh ubuntu@orb "cd ~/ultima/buildroot && make BR2_EXTERNAL=~/ultima/br2-external ultima_rpi5_defconfig"

# 3. Build (takes ~1-2 hours first time)
ssh ubuntu@orb "cd ~/ultima/buildroot && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"

# 4. Copy image
mkdir -p ~/code/ultima/output
scp ubuntu@orb:~/ultima/buildroot/output/images/sdcard.img ~/code/ultima/output/sdcard.img
```

### Output

The build produces `sdcard.img` (~400MB) with:
- 64MB FAT32 boot partition (kernel, DTB, config, firmware)
- 320MB ext4 root partition (read-only)
- 16MB ext4 data partition (persistent odometer state at `/data`)

---

## Flashing

### SD Card (macOS)

```bash
scripts/flash.sh
```

The script auto-detects external disks, shows a picker, and flashes with `dd`.

### NVMe (from running Pi)

Boot from SD card first, then:

```bash
# From the Mac, SCP the image to the Pi
scp -O output/sdcard.img root@<pi-ip>:/tmp/

# SSH into the Pi
ssh root@<pi-ip>

# Flash to NVMe
dd if=/tmp/sdcard.img of=/dev/nvme0n1 bs=4M
sync
```

Then edit cmdline.txt on the NVMe boot partition:
```bash
mount -t vfat /dev/nvme0n1p1 /boot
# Edit /boot/rpi-firmware/cmdline.txt — change root=/dev/mmcblk0p2 to root=/dev/nvme0n1p2
umount /boot
```

**Note**: Use `scp -O` (legacy SCP protocol) because Dropbear doesn't include an SFTP server. BusyBox `dd` doesn't support `status=progress`.

---

## RPi5 EEPROM Configuration

The RPi5 EEPROM controls boot behavior before the kernel loads. Edit with Raspberry Pi OS:

```bash
sudo rpi-eeprom-config --edit
```

### Recommended EEPROM Settings

```
[all]
BOOT_UART=0
BOOT_ORDER=0xf1
NET_INSTALL_AT_POWER_ON=0
HDMI_DELAY=0
DISPLAY_DIAG=0
```

| Setting | Value | Purpose |
|---------|-------|---------|
| `BOOT_UART=0` | Disable serial debug output from bootloader. Set to `1` for UART profiling |
| `BOOT_ORDER=0xf1` | SD only (1), then restart (f). Saves ~0.4s by skipping USB probe |
| `NET_INSTALL_AT_POWER_ON=0` | Disable the pink "Network Install" screen |
| `HDMI_DELAY=0` | No HDMI initialization delay |
| `DISPLAY_DIAG=0` | Suppress bootloader diagnostics on HDMI |

**Note**: For UART boot profiling, temporarily set `BOOT_UART=1`. The RPi5 UART debug output is on the JST-SH header between the HDMI ports (NOT GPIO 14/15). Use a USB UART adapter at 115200 baud.

### Boot Order Codes

| Code | Device |
|------|--------|
| `1` | SD card |
| `4` | USB mass storage |
| `6` | NVMe |
| `f` | Restart loop |

**Examples**:
- SD only → restart: `0xf1` (fastest — recommended)
- SD → USB → restart: `0xf41`
- NVMe → SD → USB: `0xf146`
- USB → SD → restart: `0xf14`

### Updating EEPROM Firmware

The default firmware channel may not respect `DISPLAY_DIAG=0`. Switch to `latest`:

```bash
sudo raspi-config
# Advanced Options → Bootloader Version → Latest
sudo rpi-eeprom-update -a
sudo reboot
```

### EEPROM Recovery Mechanism (Advanced)

You can place `recovery.bin` + `pieeprom.upd` on the FAT boot partition. The bootloader will flash the EEPROM on next boot and rename the files. However, the `recovery.bin` must match your board revision — using the wrong one causes the recovery to fail silently (recovery.bin renamed to recovery.000).

---

## SSH Access

### Setup

Place your SSH public key in the overlay:

```
br2-external/board/ultima-rpi5/overlay/root/.ssh/authorized_keys
```

Permissions must be:
- `.ssh/` directory: 700
- `authorized_keys`: 600

### Connecting

```bash
ssh root@<pi-ip>
```

The Pi gets its IP via DHCP on WiFi. Find it with:
```bash
arp -a | grep "2c:cf:67"   # RPi5 MAC prefix
```

**Note**: WiFi connects ~10-30 seconds after boot (backgrounded). SSH (Dropbear) starts even later at S50. Give the Pi about 30 seconds after the Qt app appears.

---

## Debugging

### Read Boot Logs from macOS

With the SD card in the Mac:
```bash
scripts/read-logs.sh
```

This reads `ultima-app.log`, `ultima-debug.log`, and `dmesg.log` from the FAT boot partition.

### On the Pi via SSH

```bash
# App log
cat /boot/ultima-app.log

# Kernel log
dmesg

# Check if app is running
ps | grep ultima

# Check WiFi
ip addr show wlan0
```

### Boot Partition Layout

Files are under `rpi-firmware/` subdirectory on the boot partition:
- `rpi-firmware/config.txt`
- `rpi-firmware/cmdline.txt`
- `rpi-firmware/overlays/`

The kernel Image and DTBs are at the root of the boot partition.

---

## Known Issues & Workarounds

### 1. GCC 15 Breaks Buildroot
**Problem**: Ubuntu 25.10+ ships GCC 15, which causes host-m4 1.4.19 gnulib to fail.
**Fix**: Install `gcc-14` and always build with `HOSTCC=gcc-14 HOSTCXX=g++-14`.

### 2. WiFi Firmware Not Installed by Buildroot
**Problem**: Buildroot's `BRCM_BCM43XX` + `CYPRESS_CYW43XX` don't install the correct `brcmfmac43455-sdio` files for RPi5.
**Fix**: Manually place firmware files in overlay at `lib/firmware/brcm/`.

### 3. brcmfmac Must Be a Module
**Problem**: If built-in (`=y`), the driver loads before rootfs is mounted and can't find firmware files.
**Fix**: Set `CONFIG_BRCMFMAC=m` and `CONFIG_BRCMUTIL=m` in kernel fragments. Load with `modprobe brcmfmac` in S40network.

### 4. BusyBox modprobe Can't Handle .ko.xz
**Problem**: Kernel module compression creates `.ko.xz` files that BusyBox can't decompress.
**Fix**: Set `CONFIG_MODULE_COMPRESS_NONE=y` in kernel fragments.

### 5. VC4 Forced to Module Without Sound
**Problem**: The VC4 DRM driver depends on `SND && SND_SOC`. Without these set to `=y`, VC4 becomes `=m`.
**Fix**: Set `CONFIG_SOUND=y`, `CONFIG_SND=y`, `CONFIG_SND_SOC=y`.

### 6. RPi5 Bootloader Diagnostic Screen
**Problem**: The bootloader shows a diagnostic screen on HDMI for 1-2 seconds before kernel loads. `DISPLAY_DIAG=0` doesn't work on older firmware.
**Fix**: Update to the latest EEPROM firmware via `raspi-config`. This is a cosmetic issue — the Qt app appears shortly after.

### 7. USB Boot Is Slow
**Problem**: The RPi5 bootloader retries SD → USB-MSD multiple times (~60s) before detecting USB drives.
**Fix**: Use SD card or NVMe boot. Set EEPROM `BOOT_ORDER` to try the right device first.

### 8. Dropbear Has No SFTP
**Problem**: `scp` fails with "sftp-server not found".
**Fix**: Use `scp -O` (legacy SCP protocol).

### 9. EEPROM Recovery May Fail
**Problem**: The `recovery.bin` from the rpi-eeprom GitHub repo may not match your board revision, causing silent failure.
**Fix**: Use Raspberry Pi OS with `rpi-eeprom-config --edit` instead.

### 10. PNG Images Not Showing
**Problem**: Background or needle images don't render on the device.
**Fix**: Ensure `BR2_PACKAGE_QT5BASE_PNG=y` is set in the defconfig. BMP is built-in to Qt but PNG requires explicit enablement.

### 11. Touchscreen Not Responding
**Problem**: USB touchscreen (Waveshare) detected but Qt doesn't receive touch events via libinput.
**Fix**: Set `QT_QPA_EGLFS_NO_LIBINPUT=1` in S11app to use evdev touch handler instead. Also add udev rule `99-input.rules` with `SUBSYSTEM=="input", MODE="0666"`.

### 12. Boot Partition File Layout
**Problem**: The `post-image.sh` script places config files under `rpi-firmware/` on the boot partition, not at the root.
**Note**: The RPi5 bootloader handles this correctly. When editing files on a mounted boot partition, check both root and `rpi-firmware/` subdirectory.

### 13. Read-Only Root Breaks Display If Done Wrong
**Problem**: Removing the `mount -a` (which includes `remount,rw /`) from inittab's sysinit or replacing the default fstab without including sysfs/devpts/run prevents VC4 DRM from initializing, causing a black screen.
**Fix**: The inittab overlay must keep `mount -a` in sysinit — this temporarily remounts root `rw` so sysfs/devpts/run can mount. S00remountro then flips root back to `ro` before any init scripts run. The fstab overlay must include ALL default entries plus `/var` as tmpfs. Removing `swapon -a` and getty entries is safe (no swap or interactive console needed).

### 14. Disable USB0 Device Tree Overlay
**File**: `br2-external/board/ultima-rpi5/disable-usb0.dts`

The RPi5 has two USB controllers (rp1_usb0 and rp1_usb1). The touchscreen is on usb1, so usb0 is disabled via a custom DT overlay to save boot time. The overlay is compiled by `post-build.sh` using the host DTC.

### 15. CMA Cannot Go Below 320MB
**Problem**: Setting `cma=64M` in cmdline.txt causes `raspberrypi-clk: probe failed with error -12 (ENOMEM)`, breaking the vc4 DRM display driver entirely.
**Fix**: Leave CMA at its default 320MB. The RPi5 firmware clock driver allocates from CMA during probe.

### 16. App-as-PID-1 Does Not Work
**Problem**: Running the Qt app as PID 1 (replacing BusyBox init) fails because vc4 DRM needs udev coldplug to complete device node creation. The app starts before `/dev/dri/card1` exists, and since PID 1 can't exit, the kernel panics.
**Fix**: Use BusyBox init with early app launch in S00remountro (before udev). This is nearly as fast without the fragility.

### 17. VC4 HDMI Deferred Probe Without Built-in I2C
**Problem**: The RPi5 defconfig has `CONFIG_I2C_BRCMSTB=m` (module). The VC4 HDMI driver calls `of_find_i2c_adapter_by_node()` for the DDC bus during `vc4_hdmi_bind()`, which returns `-EPROBE_DEFER` if the I2C module hasn't been loaded yet. This causes the component framework to bind HVS, fail on HDMI, unbind, and retry — 18+ cycles over ~1 second.
**Fix**: Set `CONFIG_I2C=y` and `CONFIG_I2C_BRCMSTB=y` in kernel fragments. The I2C adapter registers during early platform init, and VC4 binds on the first attempt.

### 18. Serial Console Adds ~1.7s to Boot
**Problem**: With `console=ttyAMA10,115200 loglevel=7`, the kernel flushes ~6KB of buffered early-boot printk messages over 115200 baud serial when the PL011 console is registered (~0.7s). Additionally, every subsequent printk blocks on serial transmission throughout boot, adding another ~1s cumulatively.
**Fix**: Use `loglevel=1 quiet` to suppress non-emergency messages. The UART console remains registered for userspace output (e.g., APP_LAUNCH marker). Higher baud rates (460800, 921600) are not supported by the RPi5 JST-SH debug probe.

### 19. Odometer Data Loss Window
**Problem**: Odometer state saves every 30 seconds. A power cut could lose up to 30 seconds of driving data.
**Acceptable**: This is by design — more frequent saves would wear the SD card. The data partition uses ext4 journaling to protect against filesystem corruption.
