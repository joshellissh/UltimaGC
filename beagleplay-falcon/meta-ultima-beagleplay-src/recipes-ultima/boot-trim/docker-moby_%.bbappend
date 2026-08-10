# Reference-image bloat from packagegroup-arago-base, irrelevant to a
# single-purpose gauge cluster. Disabling rather than PACKAGE_EXCLUDE-ing:
# these come in via a packagegroup's RDEPENDS, not a direct IMAGE_INSTALL
# entry, so excluding the package outright risks an unsatisfied-dependency
# do_rootfs failure; disabling the unit is the safe, standard per-recipe
# override and doesn't touch package resolution at all. Not thought to be
# serialized ahead of ultima-app.service (both are pulled in by
# multi-user.target/sockets.target with no ordering between them), but it's
# one less thing running during boot.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
