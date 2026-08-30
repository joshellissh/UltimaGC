# meta-arago-distro's tisdk-uenv recipe (pulled into every arago image
# unconditionally via arago.conf, not machine-scoped) ships a uEnv-sk.txt
# only for TI's own EVM machine name (j722s-evm), never extended to
# BeagleBoard.org's community machine name (beagley-ai) that meta-ti's
# meta-beagle layer adds — a TI SDK convenience recipe wired to EVM names
# only.
#
# FILESEXTRAPATHS is NOT auto-extended just by this bbappend existing —
# ${THISDIR} in meta-ti-foundational's own bbappend hardcodes to *its*
# directory (immediate `:=` expansion at parse time), so it never picks up
# other layers. We have to add our own directory to FILESPATH explicitly,
# which then lets bitbake's automatic per-override subdirectory search find
# tisdk-uenv/beagley-ai/uEnv-sk.txt (added alongside this file) since
# "beagley-ai" is an active MACHINE override for this build.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
