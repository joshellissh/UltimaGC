#!/bin/sh
# /data is partition 3 on whatever disk root itself booted from. It is
# deliberately NOT identified by filesystem UUID or LABEL here: every card
# dd'd from the same wic image (see wic/ultima-beagley-ai.wks.in) carries a
# byte-identical UUID/LABEL, so a UUID= or LABEL= fstab entry would be
# ambiguous the moment more than one such card is visible at once. Deriving
# the device from the already-resolved root mount sidesteps that instead of
# needing to keep multiple cards' identifiers distinct.
set -e

root_dev=$(findmnt -no SOURCE /)
case "$root_dev" in
    /dev/mmcblk0p2|/dev/mmcblk1p2) data_dev="${root_dev%p2}p3" ;;
    *)
        echo "ultima-data-mount: unexpected root device '$root_dev', expected /dev/mmcblk0p2 (eMMC) or /dev/mmcblk1p2 (SD)" >&2
        exit 1
        ;;
esac

[ -b "$data_dev" ] || { echo "ultima-data-mount: $data_dev does not exist" >&2; exit 1; }

exec mount "$data_dev" /data
