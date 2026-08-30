# BeagleY-AI boot time (2026-08-30): the GPU path (eglfs_kms via Mesa's GBM +
# the PowerVR DRI shim) was mapping libLLVM.so.18.1 — 100 MB — into
# ultima-app at startup, plus a 16 MB tidss_dri.so linked against it. Reading
# that library cold off the SD card alone measured 1.03 s on the board, all
# of it inside QGuiApplication creation. LLVM is only there for llvmpipe /
# the LLVM `draw` path of gallium's software rasterizers and for the
# r300/nouveau drivers meta-arago's `gallium-llvm` PACKAGECONFIG drags in
# (arago.conf: PACKAGECONFIG:append:pn-mesa-pvr = "gallium-llvm"). The pvr
# gallium driver is a hardware driver and does not need it; neither do
# zink/virgl, the swrast Vulkan driver or the VA/VDPAU video codecs. Keep
# mesa-pvr to exactly the PowerVR driver.
PACKAGECONFIG:remove = "gallium-llvm zink virgl vulkan video-codecs"

# mesa.inc seeds GALLIUMDRIVERS with "swrast"; mesa-pvr's own recipe then
# appends ",pvr" (after this bbappend is parsed), so this leaves exactly
# "pvr". kms_swrast/swrast were never used on the dash — the display path
# is tidss + PowerVR or nothing.
GALLIUMDRIVERS = ""
