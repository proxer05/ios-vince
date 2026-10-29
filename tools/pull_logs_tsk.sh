#!/bin/sh
# Pull the iOS unified log from a phone sitting in TWRP, without a 16 GiB dd.
#
#   tools/pull_logs_tsk.sh OUTDIR [SERIAL]
#
# Pushes the static SleuthKit binaries into TWRP, extracts
# /private/var/db/{diagnostics,uuidtext} on-device, and streams a tarball back.
# Then unpack and feed it to tools/unified_log.sh (or parse directly).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:?usage: $0 OUTDIR [SERIAL]}
SERIAL=${2:-}
TSK=$ROOT/artifacts/tsk-build/sleuthkit-4.12.1/tools/fstools
FLS=$TSK/fls-static
ICAT=$TSK/icat-static
DEV=/dev/block/mmcblk0p60

[ -x "$FLS" ] || { echo "error: missing $FLS (build SleuthKit first)" >&2; exit 1; }
[ -x "$ICAT" ] || { echo "error: missing $ICAT" >&2; exit 1; }

ADB="adb"
[ -n "$SERIAL" ] && ADB="adb -s $SERIAL"

mkdir -p "$OUT"

echo "[*] Pushing extractor to TWRP"
$ADB push "$FLS" /tmp/fls-static >/dev/null
$ADB push "$ICAT" /tmp/icat-static >/dev/null
$ADB push "$ROOT/tools/tsk_extract.sh" /tmp/tsk_extract.sh >/dev/null
$ADB shell 'chmod 755 /tmp/fls-static /tmp/icat-static /tmp/tsk_extract.sh'

echo "[*] Extracting diagnostics + uuidtext on-device"
$ADB shell "sh /tmp/tsk_extract.sh $DEV /tmp/fls-static /tmp/icat-static /tmp/vince-logs"

echo "[*] Streaming tarball back"
$ADB exec-out 'tar cf - -C /tmp vince-logs' > "$OUT/vince-logs.tar"

echo "[*] Unpacking"
tar xf "$OUT/vince-logs.tar" -C "$OUT"
ls -l "$OUT/vince-logs" 2>/dev/null
echo "[*] Done: $OUT/vince-logs/{diagnostics,uuidtext}"
echo "    Parse with: tools/unified_log.sh IMAGE OUTDIR   (or reuse the flat-layout step)"
