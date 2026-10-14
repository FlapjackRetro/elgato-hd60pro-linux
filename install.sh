#!/bin/bash
# Installs the hd60pro driver with DKMS so it is rebuilt automatically on kernel updates.
# Usage: sudo bash install.sh        (sudo bash install.sh --remove to uninstall)
set -e
cd "$(dirname "$0")"
NAME=hd60pro
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
[ "$(id -u)" = 0 ] || { echo "Please run with sudo"; exit 1; }

remove_all() {
	# detach the card first so sound servers (WirePlumber/PulseAudio) let go of it
	for dev in /sys/bus/pci/drivers/$NAME/0000:*; do
		[ -e "$dev" ] && echo "$(basename $dev)" > /sys/bus/pci/drivers/$NAME/unbind 2>/dev/null
	done
	for i in 1 2 3 4 5 6 7 8 9 10; do
		lsmod | grep -q "^$NAME " || break
		modprobe -r $NAME 2>/dev/null || true
		sleep 1
	done
	for v in $(dkms status $NAME 2>/dev/null | sed -n 's/^[^/]*\/\([^,:]*\).*/\1/p' | sort -u); do
		dkms remove $NAME/$v --all || true
		rm -rf /usr/src/$NAME-$v
	done
}

if [ "${1:-}" = "--remove" ]; then
	remove_all
	rm -f /etc/modules-load.d/$NAME.conf
	echo "$NAME removed."
	exit 0
fi

command -v dkms >/dev/null || apt-get install -y dkms
[ -d /lib/modules/$(uname -r)/build ] || apt-get install -y linux-headers-$(uname -r)

remove_all
mkdir -p /usr/src/$NAME-$VER
cp hd60pro.c Makefile dkms.conf /usr/src/$NAME-$VER/
dkms add $NAME/$VER
dkms build $NAME/$VER
dkms install $NAME/$VER
echo $NAME > /etc/modules-load.d/$NAME.conf
modprobe $NAME
sleep 3
for v in /sys/class/video4linux/video*; do
	grep -q "Elgato HD60 Pro" "$v/name" 2>/dev/null && echo "Installed: capture card is /dev/$(basename $v)"
done
dmesg | grep hd60pro | tail -5
