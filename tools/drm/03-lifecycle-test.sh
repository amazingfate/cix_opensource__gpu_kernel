#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 03-lifecycle-test.sh - run one render node lifecycle case on the board.
#
# Usage:
#   sudo tools/drm/03-lifecycle-test.sh <case> [output-dir]
#
# Cases:
#   loop         1000 open/close cycles
#   multi        16 processes holding the node at the same time
#   fork         parent keeps the node while forked children use and close it
#   unbind       hold an fd, unbind the platform device, expect ENODEV on the
#                held fd; MUST be the only unbind in this boot and should be
#                followed by a reboot
#   reset        hold an fd, trigger a GPU reset through the mali debugfs
#                "reset" file, check the fd and the node still work
#   suspend      arm the RTC, suspend to RAM, verify the node after resume
#
# Every case records dmesg before/after and fails if a new WARNING/BUG/oops
# appears.  The board is expected to be back in its normal state (mali_kbase
# loaded, panthor blacklisted) before running a case.

set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CASE=${1:?usage: $0 <loop|multi|fork|unbind> [output-dir]}
OUT=${2:-./lifecycle-$CASE-$(date +%Y%m%d-%H%M%S)}
NODE=/dev/dri/renderD128
PLATDEV=CIXH5000:00

mkdir -p "$OUT"

[ -x "$SCRIPT_DIR/drm-hold" ] || ( cd "$SCRIPT_DIR" && make >/dev/null )
[ -x "$SCRIPT_DIR/drm-info" ] || ( cd "$SCRIPT_DIR" && make >/dev/null )

if [ ! -c "$NODE" ]; then
	echo "FAIL: $NODE does not exist (is mali_kbase loaded?)" | tee "$OUT/result.txt"
	exit 1
fi

dmesg_snapshot() { dmesg 2>/dev/null | grep -cE "WARNING:|BUG:|Oops|KASAN|refcount_t"; }

before=$(dmesg_snapshot)
rc=0

case "$CASE" in
loop)
	fail=0
	for i in $(seq 1 1000); do
		"$SCRIPT_DIR/drm-info" "$NODE" >/dev/null 2>&1 || fail=$((fail + 1))
	done
	{
		echo "case: loop"
		echo "open/close cycles: 1000"
		echo "failures: $fail"
	} >"$OUT/result.txt"
	[ "$fail" -eq 0 ] || rc=1
	;;
multi)
	{
		echo "case: multi"
		for i in $(seq 1 16); do
			"$SCRIPT_DIR/drm-hold" "$NODE" 2000 &
		done
		wait
	} >"$OUT/result.txt" 2>&1
	grep -q "ioctl=ok" "$OUT/result.txt" || rc=1
	;;
fork)
	"$SCRIPT_DIR/drm-hold" "$NODE" 1500 --fork-close --fork-children 4 \
		>"$OUT/result.txt" 2>&1
	grep -q "ioctl=ok" "$OUT/result.txt" || rc=1
	;;
unbind)
	"$SCRIPT_DIR/drm-hold" "$NODE" 6000 >"$OUT/result.txt" 2>&1 &
	hold_pid=$!
	sleep 2
	driver_path=/sys/bus/platform/devices/$PLATDEV/driver
	if [ ! -e "$driver_path" ]; then
		echo "FAIL: $PLATDEV is not bound to a driver" | tee -a "$OUT/result.txt"
		kill "$hold_pid" 2>/dev/null
		exit 1
	fi
	echo "$PLATDEV" > "$driver_path/unbind"
	wait "$hold_pid"
	{
		echo "case: unbind"
		echo "node present after unbind: $([ -c "$NODE" ] && echo yes || echo no)"
	} >>"$OUT/result.txt"
	# The held fd must see ENODEV (19) on its ioctl, and close cleanly.
	grep -q "ioctl=errno:19" "$OUT/result.txt" || rc=1
	[ -c "$NODE" ] && rc=1
	;;
reset)
	"$SCRIPT_DIR/drm-hold" "$NODE" 8000 >"$OUT/result.txt" 2>&1 &
	hold_pid=$!
	sleep 2
	echo 1 > /sys/kernel/debug/mali0/reset
	wait "$hold_pid"
	{
		echo "case: reset"
		echo "node present after reset: $([ -c "$NODE" ] && echo yes || echo no)"
	} >>"$OUT/result.txt"
	grep -q "ioctl=ok" "$OUT/result.txt" || rc=1
	[ -c "$NODE" ] || rc=1
	;;
suspend)
	# Arm the RTC as a wake source, then suspend. The script continues after
	# resume; if the board does not come back it needs a power cycle.
	# Run it detached (setsid/nohup) so an SSH drop during suspend does not
	# kill it, and capture /dev/ttyUSB0 on the host to see where it stops.
	# SLEEP_MODE=s2idle (default) or deep.
	# rtc0 is the EFI RTC (rtc-efi, no usable wake alarm); rtc1 is the
	# rx8900 I2C RTC whose alarm IRQ can wake the SoC. Override with RTC_DEV.
	rtc_dev=${RTC_DEV:-/sys/class/rtc/rtc1}
	rtcwake_dev=$rtc_dev/wakealarm
	sleep_mode=${SLEEP_MODE:-s2idle}
	{
		echo "case: suspend"
		echo "mem_sleep was: $(cat /sys/power/mem_sleep)"
		echo "requested sleep mode: $sleep_mode"
	} >"$OUT/result.txt"
	echo "$sleep_mode" > /sys/power/mem_sleep
	echo 0 > "$rtcwake_dev"
	echo +60 > "$rtcwake_dev"
	wakealarm=$(cat "$rtcwake_dev")
	echo "rtc wakealarm: $wakealarm" >>"$OUT/result.txt"
	if [ -z "$wakealarm" ] || [ "$wakealarm" = "0" ]; then
		echo "FAIL: RTC wakealarm could not be armed" >>"$OUT/result.txt"
		exit 1
	fi
	echo "suspending at: $(date -Is)" >>"$OUT/result.txt"
	sync
	systemctl suspend
	echo "resumed at: $(date -Is)" >>"$OUT/result.txt"
	echo "rtc wakealarm after resume: $(cat "$rtcwake_dev")" >>"$OUT/result.txt"
	echo "node present after resume: $([ -c "$NODE" ] && echo yes || echo no)" >>"$OUT/result.txt"
	"$SCRIPT_DIR/drm-info" "$NODE" >>"$OUT/result.txt" 2>&1 || rc=1
	[ -c "$NODE" ] || rc=1
	echo deep > /sys/power/mem_sleep
	;;
*)
	echo "unknown case: $CASE" >&2
	exit 2
	;;
esac

after=$(dmesg_snapshot)
{
	echo "new kernel warnings during case: $((after - before))"
	echo "case exit: $rc"
} >>"$OUT/result.txt"

if [ "$after" -ne "$before" ]; then
	rc=1
	echo "FAIL: new kernel warnings, see dmesg" | tee -a "$OUT/result.txt"
	dmesg 2>/dev/null | tail -40 >>"$OUT/result.txt"
fi

[ "$rc" -eq 0 ] && echo "PASS: $CASE ($OUT)" || echo "FAIL: $CASE ($OUT)"
exit "$rc"
