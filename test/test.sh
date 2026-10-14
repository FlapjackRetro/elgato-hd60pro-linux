#!/bin/bash
# HD60 Pro driver test run. Builds the module, loads it, tries a short
# capture and writes everything to ./results/ for analysis.
# Usage:  cd ~/Downloads/hd60pro && sudo ./test.sh
set -u
cd "$(dirname "$0")/.."
OUT=results
mkdir -p "$OUT"
LOG="$OUT/test.log"
: > "$LOG"
say() { echo "== $*" | tee -a "$LOG"; }
run() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; return $?; }

if [ "$(id -u)" != 0 ]; then echo "Please run with sudo: sudo ./test.sh"; exit 1; fi

say "system"
run uname -a
run cat /etc/os-release
command -v mokutil >/dev/null && run mokutil --sb-state
run lspci -nnk -d 12ab:

MISSING=""
[ -d "/lib/modules/$(uname -r)/build" ] || MISSING="$MISSING linux-headers-$(uname -r)"
command -v make >/dev/null && command -v gcc >/dev/null || MISSING="$MISSING build-essential"
command -v v4l2-ctl >/dev/null || MISSING="$MISSING v4l-utils"
if [ -n "$MISSING" ]; then
	say "missing packages:$MISSING"
	echo "Install them with:  sudo apt install$MISSING   then re-run this script."
	exit 1
fi

say "build"
KDIR=/lib/modules/$(uname -r)/build
make -C "$KDIR" M="$(pwd)" clean >/dev/null 2>&1
if ! run make -C "$KDIR" M="$(pwd)" modules; then
	say "BUILD FAILED - see $LOG"
	tail -30 "$LOG"
	exit 1
fi

say "load"
rmmod hd60pro 2>/dev/null
dmesg -C
run modprobe videobuf2-vmalloc
run modprobe videobuf2-v4l2
if ! run insmod ./hd60pro.ko debug=1; then
	say "insmod failed (Secure Boot? see mokutil output above)"
	dmesg >> "$LOG"
	exit 1
fi
sleep 3
DBG=$(ls -d /sys/kernel/debug/0000:*:*.* 2>/dev/null | while read d; do [ -e "$d/regs" ] && echo "$d"; done | head -1)
say "debugfs: ${DBG:-none}"
[ -n "$DBG" ] && run cat "$DBG/regs"

if [ -n "$DBG" ]; then
	say "native signal detection"
	echo 1 > "$DBG/detect"
	run cat "$DBG/regs"
fi

DEV=$(for v in /sys/class/video4linux/video*; do grep -q "Elgato HD60 Pro" "$v/name" 2>/dev/null && echo /dev/$(basename "$v"); done | head -1)
say "video device: ${DEV:-none}"
if [ -n "$DEV" ]; then
	run v4l2-ctl -d "$DEV" --all
	run v4l2-ctl -d "$DEV" --list-formats-ext
	say "capture 120 frames"
	run timeout 20 v4l2-ctl -d "$DEV" --stream-mmap=4 --stream-count=120 --stream-to="$OUT/capture.raw" --verbose
	ls -l "$OUT/capture.raw" >> "$LOG" 2>&1
	[ -n "$DBG" ] && run cat "$DBG/regs"
	[ -n "$DBG" ] && head -c 4612096 "$DBG/frame" > "$OUT/lastbuf.raw" 2>>"$LOG"
	# keep only the first 2 frames of the capture to save space
	if [ -s "$OUT/capture.raw" ]; then
		SZ=$(v4l2-ctl -d "$DEV" --get-fmt-video 2>/dev/null | awk '/Size Image/ {print $4}')
		[ -n "$SZ" ] && head -c $((SZ*2)) "$OUT/capture.raw" > "$OUT/capture2.raw" && mv "$OUT/capture2.raw" "$OUT/capture.raw"
	fi
fi

say "kernel log"
dmesg | grep -iE "hd60|12ab|video|vb2|DMAR|AMD-Vi|iommu.*05:00" >> "$LOG"
dmesg > "$OUT/dmesg.txt"

say "unload"
run rmmod hd60pro
chmod -R a+rw "$OUT"
say "done - results are in $(pwd)/$OUT"
