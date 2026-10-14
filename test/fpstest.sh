#!/bin/bash
# Measures capture frame rate with different card settings. Run: sudo bash fpstest.sh
cd "$(dirname "$0")/.."
OUT=results; mkdir -p $OUT; LOG=$OUT/fps.log; : > $LOG
KDIR=/lib/modules/$(uname -r)/build
make -C "$KDIR" M="$(pwd)" modules > $OUT/build.log 2>&1 || { echo "build failed"; tail $OUT/build.log; exit 1; }
rmmod hd60pro 2>/dev/null; dmesg -C
modprobe videobuf2-vmalloc; modprobe videobuf2-v4l2
insmod ./hd60pro.ko debug=1 || exit 1
sleep 2
DEV=$(for v in /sys/class/video4linux/video*; do grep -q "Elgato HD60 Pro" "$v/name" 2>/dev/null && echo /dev/$(basename "$v"); done | head -1)
for bs in 1 0; do
	echo $bs > /sys/module/hd60pro/parameters/bitstreams
	echo "== bitstreams=$bs" | tee -a $LOG
	timeout 30 v4l2-ctl -d $DEV --stream-mmap=6 --stream-count=600 --stream-to=/dev/null --verbose 2>&1 | grep -oE "fps: [0-9.]+" | tail -3 | tee -a $LOG
	sleep 1
done
dmesg | grep -E "stopped|start " >> $LOG
rmmod hd60pro
chmod -R a+rw $OUT
echo "done - results in $(pwd)/$LOG"
