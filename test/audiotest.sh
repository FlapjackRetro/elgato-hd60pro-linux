#!/bin/bash
# Audio + frame-rate test for the hd60pro driver.  Run: sudo bash audiotest.sh
cd "$(dirname "$0")/.."
OUT=results; mkdir -p $OUT; LOG=$OUT/audio.log; : > $LOG
[ "$(id -u)" = 0 ] || { echo "Please run with sudo"; exit 1; }
command -v arecord >/dev/null || apt-get install -y alsa-utils
KDIR=/lib/modules/$(uname -r)/build
make -C "$KDIR" M="$(pwd)" modules > $OUT/build.log 2>&1 || { echo "build failed"; tail -20 $OUT/build.log; exit 1; }
unload() {
	# detach the card first so sound servers release the ALSA device
	for dev in /sys/bus/pci/drivers/hd60pro/0000:*; do
		[ -e "$dev" ] && echo "$(basename $dev)" > /sys/bus/pci/drivers/hd60pro/unbind 2>/dev/null
	done
	for i in 1 2 3 4 5 6 7 8 9 10; do
		lsmod | grep -q '^hd60pro ' || return 0
		modprobe -r hd60pro 2>/dev/null || rmmod hd60pro 2>/dev/null
		sleep 1
	done
	lsmod | grep -q '^hd60pro ' || return 0
	echo
	echo "The hd60pro driver is still in use and cannot be swapped for the test version."
	echo "Programs using the capture card:"
	fuser -v /dev/video* /dev/snd/* 2>&1 | grep -v "^ *$" | head -20
	echo
	echo "Please close OBS (or any other app using the card) and run this script again."
	echo "If only 'pipewire' or 'wireplumber' is listed, run:  systemctl --user restart wireplumber"
	echo "(as your normal user, not with sudo), then run this script again."
	exit 1
}
unload; dmesg -C
modprobe videobuf2-vmalloc; modprobe videobuf2-v4l2; modprobe snd-pcm
insmod ./hd60pro.ko debug=1 || { dmesg | tail -20 >> $LOG; exit 1; }
echo "waiting for the desktop sound server to finish probing the new card..."
sleep 8
DBG=$(ls -d /sys/kernel/debug/0000:*:*.* 2>/dev/null | while read d; do [ -e "$d/atrace" ] && echo "$d"; done | head -1)
DEV=$(for v in /sys/class/video4linux/video*; do grep -q "Elgato HD60 Pro" "$v/name" 2>/dev/null && echo /dev/$(basename "$v"); done | head -1)
echo "== audio only, 5 s" | tee -a $LOG
for try in 1 2 3; do
	timeout 15 arecord -D hw:HD60Pro,0 -f S16_LE -c 2 -r 48000 -d 5 $OUT/audio_only.wav >> $LOG 2>&1 && break
	echo "busy, retrying" >> $LOG; sleep 3
done
cat $DBG/atrace > $OUT/atrace_audio_only.txt
echo "== audio + video together, 4 s" | tee -a $LOG
timeout 15 v4l2-ctl -d $DEV --stream-mmap=6 --stream-count=300 --stream-to=/dev/null > $OUT/v4l2.txt 2>&1 &
sleep 1
timeout 15 arecord -D hw:HD60Pro,0 -f S16_LE -c 2 -r 48000 -d 4 $OUT/audio_video.wav >> $LOG 2>&1
wait
cat $DBG/atrace > $OUT/atrace_av.txt
for bs in 0; do
	echo $bs > /sys/module/hd60pro/parameters/bitstreams
	echo "== video only, bitstreams=$bs" | tee -a $LOG
	timeout 30 v4l2-ctl -d $DEV --stream-mmap=6 --stream-count=600 --stream-to=/dev/null 2>&1 | grep -oE "[0-9.]+ fps" | tail -2 >> $LOG
	sleep 1
done
dmesg | grep hd60pro >> $LOG
ls -l $OUT/*.wav >> $LOG
unload || true
modprobe hd60pro 2>/dev/null   # reload the installed version
chmod -R a+rw $OUT
echo "done - results in $(pwd)/$OUT"
