# ultima-app.bb lives in this same layer (recipes-ultima/ultima-app/). This
# bbappend adds the beagley-ai-specific ultima-app.service and boot-time
# pieces (below), kept separate from the board-agnostic base recipe.
#
# FILESEXTRAPATHS:prepend adds this recipe's ${PN} subdir to FILESPATH so
# bitbake's automatic per-override subdirectory search covers
# ultima-app/beagley-ai/ultima-app.service (added alongside this file), which
# then wins over the base recipe's own files/ultima-app.service for
# MACHINE=beagley-ai builds specifically. (Learned the hard way fixing the
# tisdk-uenv gap — see the BeagleY-AI notes "Yocto build / app port".)
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# Boot-time pieces (2026-08-30), beagley-ai only — see the comments in each
# file and NOTES.md "BeagleY-AI boot-time work":
# - ultima-prefetch(.service/.list) + readahead.pack: read the app's startup
#   working set into the page cache from the moment systemd starts, in
#   parallel with its early units. The pack is the page-range recording
#   (ultima-readahead, 59 files / 42 MB of ranges); the .list is the
#   whole-file fallback when no pack is present.
# - udev-trigger-after-dash.conf: drop-in ordering udev's coldplug (and all
#   the module/firmware loading it triggers) after the app's first frame,
#   which the Type=notify beagley-ai ultima-app.service reports via READY=1.
SRC_URI:append:beagley-ai = " \
    file://ultima-prefetch \
    file://ultima-prefetch.service \
    file://ultima-prefetch.list \
    file://readahead.pack \
    file://udev-trigger-after-dash.conf \
"
SYSTEMD_SERVICE:${PN}:append:beagley-ai = " ultima-prefetch.service"

do_install:append:beagley-ai () {
    install -m 0755 ${WORKDIR}/ultima-prefetch ${D}${bindir}/ultima-prefetch
    install -m 0644 ${WORKDIR}/ultima-prefetch.service ${D}${systemd_unitdir}/system/ultima-prefetch.service
    install -d ${D}${sysconfdir}/ultima
    install -m 0644 ${WORKDIR}/ultima-prefetch.list ${D}${sysconfdir}/ultima/prefetch.list
    install -m 0644 ${WORKDIR}/readahead.pack ${D}${sysconfdir}/ultima/readahead.pack
    install -d ${D}${sysconfdir}/systemd/system/systemd-udev-trigger.service.d
    install -m 0644 ${WORKDIR}/udev-trigger-after-dash.conf \
        ${D}${sysconfdir}/systemd/system/systemd-udev-trigger.service.d/ultima-after-dash.conf
}

FILES:${PN}:append:beagley-ai = " \
    ${bindir}/ultima-prefetch \
    ${systemd_unitdir}/system/ultima-prefetch.service \
    ${sysconfdir}/ultima/prefetch.list \
    ${sysconfdir}/ultima/readahead.pack \
    ${sysconfdir}/systemd/system/systemd-udev-trigger.service.d/ultima-after-dash.conf \
"
