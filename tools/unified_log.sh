#!/bin/sh
# Extract the iOS unified log from a raw HFSX root snapshot and parse it.
#
#   tools/unified_log.sh IMAGE OUTDIR [GREP_SUBSTRING]
#
# IMAGE is a raw image whose start holds the iOS HFSX root volume (e.g. a
# 16 GiB dd of /dev/block/mmcblk0p60).  For a much faster path on a live
# device, use tools/pull_logs_tsk.sh instead.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 IMAGE OUTDIR [GREP_SUBSTRING]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=$1
OUT=$2
GREP=${3:-}

mkdir -p "$OUT/raw"

echo "[*] Extracting diagnostics and uuidtext from $IMAGE"
docker run --rm --privileged --entrypoint bash \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$IMAGE":/image:ro -v "$OUT/raw":/out iosroot-tools -c '
set -e
loop=$(losetup -f --show -r /image)
trap "umount /mnt 2>/dev/null || true; losetup -d \"$loop\"" EXIT
mount -t hfsplus -o ro,force "$loop" /mnt
mkdir -p /out
cp -a /mnt/private/var/db/diagnostics /out/
cp -a /mnt/private/var/db/uuidtext /out/
chown -R "$HOST_UID:$HOST_GID" /out
'

exec "$ROOT/tools/parse_unified_log.sh" "$OUT/raw" "$OUT" "$GREP"
