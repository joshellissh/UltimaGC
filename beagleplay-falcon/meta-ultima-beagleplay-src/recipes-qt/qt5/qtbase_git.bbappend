# BeaglePlay has no usable open GPU driver for Qt5's EGL/GLES path yet
# (PowerVR AXE-1-16M — same situation as the Buildroot BeaglePlay port, see
# UltimaGC's SETUP-BEAGLEPLAY.md "Rendering: Why Software, Not GPU"). qtbase's
# default PACKAGECONFIG_GL pulls in eglfs/kms/gbm/gles2 whenever DISTRO_FEATURES
# has opengl (which arago's does) and x11 doesn't — drop that and build
# linuxfb instead, scoped to this machine only (a global local.conf override
# broke unrelated recipes when meta-falcon-beagleplay hit the same class of
# issue — see beagleplay-falcon/NOTES.md).
PACKAGECONFIG:remove:beagleplay-ti = "eglfs kms gbm gles2"
PACKAGECONFIG:append:beagleplay-ti = " no-opengl linuxfb"
