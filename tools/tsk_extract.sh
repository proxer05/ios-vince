#!/sbin/sh
# On-device HFS+ extractor for TWRP.  Uses static SleuthKit fls/icat to read
# the iOS root straight off the block device, so TWRP never has to mount HFS+.
#
#   tsk_extract.sh IMAGE FLS ICAT OUTDIR
#
# Extracts OUTDIR/diagnostics and OUTDIR/uuidtext only.
set -eu

IMG=$1
FLS=$2
ICAT=$3
OUT=$4

walk() {
    ino=$1
    shift
    for n in "$@"; do
        next=$("$FLS" -f hfs -p "$IMG" "$ino" 2>/dev/null |
            awk -v n="$n" '$NF == n { sub(/:$/, "", $2); print $2; exit }')
        if [ -z "$next" ]; then
            echo "tsk_extract: cannot find '$n' under inode $ino" >&2
            exit 1
        fi
        ino=$next
    done
    echo "$ino"
}

extract() {
    root=$1
    dest=$2
    "$FLS" -f hfs -r -p "$IMG" "$root" 2>/dev/null |
    while IFS= read -r line; do
        type=$(printf '%s\n' "$line" | awk '{ print $1 }')
        ino=$(printf '%s\n' "$line" | awk '{ sub(/:$/, "", $2); print $2 }')
        path=$(printf '%s\n' "$line" | cut -f2)
        case $ino in ''|*[!0-9]*) continue ;; esac
        case $type in
            d/*)
                mkdir -p "$dest/$path"
                ;;
            r/*)
                mkdir -p "$dest/$(dirname "$path")"
                "$ICAT" -f hfs "$IMG" "$ino" > "$dest/$path" 2>/dev/null || true
                ;;
        esac
    done
}

db=$(walk 2 private var db)
diag=$(walk "$db" diagnostics)
uuid=$(walk "$db" uuidtext)

echo "tsk_extract: diagnostics inode $diag, uuidtext inode $uuid" >&2
rm -rf "$OUT"
mkdir -p "$OUT"
extract "$diag" "$OUT/diagnostics"
extract "$uuid" "$OUT/uuidtext"
echo "tsk_extract: done" >&2
