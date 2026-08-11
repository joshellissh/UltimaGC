FILESEXTRAPATHS:prepend := "${THISDIR}/linux-ti-staging:"
SRC_URI:append = " file://ultima-can.cfg file://ultima-display.cfg file://ultima-no-wifi.cfg"

# This recipe's do_configure comes from setup-defconfig.inc, not the
# generic kernel-yocto class — it does NOT auto-merge every *.cfg in
# SRC_URI. It only merges what's listed in KERNEL_CONFIG_FRAGMENTS
# (absolute paths, see that file's "Fourth, handle config fragments
# specified in the recipe" block). Confirmed the hard way: the SRC_URI
# line alone fetched the fragment into WORKDIR but the built .config still
# had CONFIG_CAN_GS_USB unset.
KERNEL_CONFIG_FRAGMENTS += "${WORKDIR}/ultima-can.cfg ${WORKDIR}/ultima-display.cfg ${WORKDIR}/ultima-no-wifi.cfg"
