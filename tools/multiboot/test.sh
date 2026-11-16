#!/bin/sh
# Host test of rootfs/etc/init.d/S01multiboot (multiboot card detection): ./tools/multiboot/test.sh
cd "$(dirname "$0")/../.." || exit 1
T=$(mktemp -d)
fail=0
case_() {   # case_ <name> <cmdline> <expected line, empty: not multiboot>
    echo "$2" > "$T/cmdline"
    rm -f "$T/out"
    MB_CMDLINE=$T/cmdline MB_BOOTDEV=$T/none MB_OUT=$T/out sh rootfs/etc/init.d/S01multiboot start >/dev/null
    got=$(cat "$T/out" 2>/dev/null)
    if [ "$got" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$got', want '$3'"; fail=1; fi
}
case_ "own card"            "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10" ""
case_ "menu marker"         "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait x6100mb=1" "boot menu key F1"
case_ "marker, system 3"    "root=/dev/mmcblk0p6 rootwait x6100mb=3" "boot menu key F3"
case_ "root not p2"         "console=ttyS0,115200 root=/dev/mmcblk0p5 rootwait panic=10" "root on partition 5"
case_ "root p12"            "root=/dev/mmcblk0p12 rootwait" "root on partition 12"
case_ "similar names"       "root=/dev/mmcblk0p2 nox6100mb=4 myroot=/dev/mmcblk0p5" ""
case_ "first argument"      "x6100mb=2 root=/dev/mmcblk0p2" "boot menu key F2"
rm -rf "$T"
exit $fail
