#!/bin/sh
# Diagnostic init wrapper - runs before real init
# Shows if rootfs mounted and basic system state

exec > /dev/tty1 2>&1

echo ""
echo "================================"
echo "  ULTIMA INIT DIAGNOSTIC"
echo "================================"
echo ""
echo "If you see this, rootfs mounted OK"
echo ""
echo "--- partitions ---"
cat /proc/partitions
echo ""
echo "--- mounts ---"
cat /proc/mounts
echo ""
echo "--- block devs ---"
ls /dev/sd* /dev/mmcblk* /dev/nvme* 2>&1
echo ""
echo "--- /sbin/init ---"
ls -la /sbin/init
echo ""
echo "--- busybox ---"
ls -la /bin/busybox
echo ""
echo "Continuing to real init in 30 seconds..."
echo "(read the info above)"
sleep 30

exec /sbin/init
