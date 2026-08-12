# GPU enablement (2026-08-12): meta-ti's beagleplay-ti.conf already wires
# PREFERRED_PROVIDER_virtual/{egl,libgles2,libgbm} to mesa-pvr and arago's
# DISTRO_FEATURES already has opengl (no x11) — so qtbase's *default*
# PACKAGECONFIG_GL for this machine already resolves to "kms gbm gles2
# eglfs" with no override needed. This bbappend used to strip that back to
# linuxfb-only on the theory that BeaglePlay "has no usable open GPU driver"
# (PowerVR AXE-1-16M) — true for the mainline drm/imagination path (Qt 5.15
# needs GLES2, and Mesa only gets GLES on PowerVR via Zink, which needs a
# Mesa two majors ahead of what this Yocto release ships), but TI's own
# rogue DDK + umlibs + mesa-pvr shim is a separate, already-version-matched
# stack meta-ti provides for exactly this machine. See NOTES.md "PowerVR GPU
# enablement" for the full investigation.
#
# Keep both QPA plugins built rather than picking one: linuxfb stays
# available as a same-image fallback (systemd unit env var only, no
# rebuild) if the GPU path regresses boot time or turns out unstable.
PACKAGECONFIG:append:beagleplay-ti = " linuxfb"
