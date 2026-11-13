#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 01-current-behavior.sh - capture the DRM baseline of a Sky1 board.
#
# Usage:
#   sudo tools/drm/01-current-behavior.sh [output-dir]
#   sudo tools/drm/01-current-behavior.sh --trace glmark2-wayland [-- output-dir]
#
# The first form only snapshots the device model. The --trace form also runs
# the given command under strace (default: glmark2-wayland) and summarises the
# DRM ioctls that were issued, split by device node.
#
# Nothing here modifies the system.

set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TRACE_CMD=(glmark2-wayland)
RUN_TRACE=0
OUT=

while [ $# -gt 0 ]; do
	case "$1" in
	--trace)
		RUN_TRACE=1
		shift
		if [ $# -gt 0 ] && [ "$1" != "--" ]; then
			TRACE_CMD=("$@")
			break
		fi
		;;
	--)
		shift
		break
		;;
	-h | --help)
		sed -n '2,16p' "$0"
		exit 0
		;;
	*)
		OUT=$1
		shift
		;;
	esac
done

OUT=${OUT:-./drm-baseline-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

log() { printf '%s\n' "$*" | tee -a "$OUT/summary.txt" >/dev/null; }

log "== baseline captured at $(date -Is) on $(uname -a)"

# 1. Device nodes and the sysfs view of them.
{
	ls -l /dev/dri/ 2>&1
	echo
	ls -l /dev/mali* 2>&1
} >"$OUT/01-dev-nodes.txt" 2>&1
log "-- /dev/dri:"; cat "$OUT/01-dev-nodes.txt" | sed 's/^/   /' >>"$OUT/summary.txt"

ls -l /sys/class/drm/ >"$OUT/02-sysfs-drm.txt" 2>&1
for d in /sys/class/drm/*; do
	printf '%s -> %s\n' "$d" "$(readlink -f "$d/device" 2>/dev/null || echo '<no device>')"
done >"$OUT/03-drm-device-parents.txt" 2>&1

lsmod >"$OUT/04-lsmod.txt" 2>&1
grep -E 'mali|panthor|drm' "$OUT/04-lsmod.txt" >"$OUT/05-lsmod-gpu.txt" 2>&1

# 2. DRM identity and capability probe, straight from the ioctls.
if [ ! -x "$SCRIPT_DIR/drm-info" ]; then
	( cd "$SCRIPT_DIR" && make >/dev/null 2>&1 ) || \
		echo "warning: could not build drm-info" >&2
fi

nodes=$(ls /dev/dri/renderD* /dev/dri/card* /dev/dri/accel* 2>/dev/null)
if [ -n "$nodes" ] && [ -x "$SCRIPT_DIR/drm-info" ]; then
	# shellcheck disable=SC2086
	"$SCRIPT_DIR/drm-info" $nodes >"$OUT/06-drm-info.txt" 2>&1
	cat "$OUT/06-drm-info.txt" >>"$OUT/summary.txt"
else
	echo "no DRM nodes found (or drm-info unavailable)" | tee "$OUT/06-drm-info.txt"
fi

# 3. Optional: what does a real client actually open and which ioctls does it
#    issue? Distinguishes discovery, buffer sharing and GPU submission.
if [ "$RUN_TRACE" = 1 ]; then
	tracelog="$OUT/07-strace"
	log "-- tracing: ${TRACE_CMD[*]}"
	if ! command -v strace >/dev/null 2>&1; then
		echo "strace not installed, skipping trace" >"$OUT/07-trace-skipped.txt"
	else
		strace -ff -yy -o "$tracelog" \
			-e trace=openat,openat2,ioctl,close,mmap \
			"${TRACE_CMD[@]}" >"$OUT/07-trace-stdout.txt" 2>&1
		cat "$OUT"/07-strace* >"$OUT/08-strace-all.txt" 2>/dev/null

		{
			echo "== DRM ioctls issued, by node"
			grep -h 'ioctl(' "$OUT"/08-strace-all.txt | \
				grep -oE '/dev/dri/[a-zA-Z0-9]+|DRM_IOCTL_[A-Z0-9_]+' | \
				paste - - 2>/dev/null | \
				sort | uniq -c | sort -rn
			echo
			echo "== nodes opened"
			grep -h 'openat.*"/dev/dri' "$OUT"/08-strace-all.txt | \
				grep -oE '/dev/dri/[a-zA-Z0-9]+' | sort | uniq -c
		} >"$OUT/09-drm-ioctl-summary.txt" 2>&1
		cat "$OUT/09-drm-ioctl-summary.txt" >>"$OUT/summary.txt"
	fi
fi

echo
echo "baseline written to $OUT"
echo "attach $OUT/summary.txt and $OUT/09-drm-ioctl-summary.txt to the issue"
