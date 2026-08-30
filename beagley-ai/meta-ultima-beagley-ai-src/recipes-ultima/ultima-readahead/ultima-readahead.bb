SUMMARY = "Record/replay page-cache readahead for the gauge cluster's boot (BeagleY-AI)"
DESCRIPTION = "See files/ultima-readahead.c. Replayed by ultima-prefetch at boot from \
/etc/ultima/readahead.pack (shipped by the ultima-app beagley-ai bbappend); \
the record side is a bench tool, run by hand right after a first frame on a \
boot where ultima-prefetch.service was masked."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

COMPATIBLE_MACHINE = "beagley-ai"

SRC_URI = "file://ultima-readahead.c"
S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -O2 -Wall ${WORKDIR}/ultima-readahead.c -o ultima-readahead
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ultima-readahead ${D}${bindir}/ultima-readahead
}
