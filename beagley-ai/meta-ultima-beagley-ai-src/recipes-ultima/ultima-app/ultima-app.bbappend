# ultima-app.bb itself lives in meta-ultima-beagleplay (shared, board-agnostic
# recipe) — this bbappend only exists to let this layer contribute a
# beagley-ai-specific ultima-app.service without touching that layer.
#
# FILESEXTRAPATHS is NOT auto-extended just by a bbappend existing for a
# recipe in another layer — ${THISDIR} immediate-expands (`:=`) to *this*
# layer's own directory, so we have to add it explicitly. (Learned the hard
# way fixing the tisdk-uenv gap — see beagleplay-falcon/NOTES.md "BeagleY-AI
# bring-up" for the full story.) This makes bitbake's automatic per-override
# subdirectory search also cover
# ultima-app/beagley-ai/ultima-app.service (added alongside this file),
# which then wins over meta-ultima-beagleplay's own
# recipes-ultima/ultima-app/files/ultima-app.service for MACHINE=beagley-ai
# builds specifically.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
