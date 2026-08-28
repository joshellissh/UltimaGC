# Distinct from beagleplay-ti's hostname ("ultimagc") deliberately — both
# machines may coexist on the same network during the BeagleY-AI
# bring-up/evaluation period, and this project already has one documented
# mDNS/host-key collision scare from two boards sharing a hostname (see
# beagleplay-falcon/NOTES.md "SSH host key" / eMMC+SD beagleplay-ti.local
# collision).
hostname:beagley-ai = "ultimagc-beagley"
