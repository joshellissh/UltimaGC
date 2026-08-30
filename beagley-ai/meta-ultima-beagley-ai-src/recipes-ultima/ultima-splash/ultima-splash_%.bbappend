# beagley-ai-specific ultima-splash.service — same FILESEXTRAPATHS trick as the
# ultima-app bbappend in this layer (the per-override subdir only gets
# searched if this layer's directory is on the path).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
