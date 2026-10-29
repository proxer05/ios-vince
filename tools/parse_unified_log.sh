#!/bin/sh
# Parse an already-extracted iOS unified log (diagnostics + uuidtext).
#
#   parse_unified_log.sh RAWDIR OUTDIR [GREP_SUBSTRING]
#
# RAWDIR must contain diagnostics/ and uuidtext/ (the on-device layout, e.g.
# the output of tools/pull_logs_tsk.sh).  Builds the flat .logarchive layout
# the parser requires, parses it, and prints a summary.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 RAWDIR OUTDIR [GREP_SUBSTRING]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RAW=$1
OUT=$2
GREP=${3:-}
PARSER=$ROOT/artifacts/ulogs/examples/target/release/unifiedlog_iterator

[ -d "$RAW/diagnostics" ] || { echo "error: $RAW/diagnostics missing" >&2; exit 1; }
[ -d "$RAW/uuidtext" ] || { echo "error: $RAW/uuidtext missing" >&2; exit 1; }
[ -x "$PARSER" ] || { echo "error: parser missing: $PARSER" >&2; exit 1; }

mkdir -p "$OUT"
LA=$OUT/logarchive
rm -rf "$LA"
mkdir -p "$LA"
for d in Persist Special Signpost HighVolume timesync; do
    [ -d "$RAW/diagnostics/$d" ] && cp -a "$RAW/diagnostics/$d" "$LA/$d"
done
for d in "$RAW"/uuidtext/*/; do
    [ -d "$d" ] && cp -a "$d" "$LA/$(basename "$d")"
done

echo "[*] Parsing unified log"
"$PARSER" -m log-archive -i "$LA" -o "$OUT/unified.jsonl" -f jsonl \
    2>"$OUT/parser.err" >/dev/null || true

python3 - "$OUT" "$GREP" <<'PY'
import json, sys, collections

out, pattern = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "")
procs = collections.Counter()
boots = collections.Counter()
hits = []
first = last = None
unresolved = total = 0

with open(out + "/unified.jsonl", "r", errors="replace") as f:
    for line in f:
        if '"message"' not in line:
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        total += 1
        t = d.get("timestamp") or ""
        if first is None or t < first:
            first = t
        if last is None or t > last:
            last = t
        sub = d.get("subsystem") or "?"
        procs[sub] += 1
        boots[d.get("boot_uuid") or "?"] += 1
        m = d.get("message") or ""
        if m == "Unknown shared string message":
            unresolved += 1
        if pattern and pattern in m:
            hits.append((t, sub, d.get("category") or "", m))

lines = [
    "messages: %d" % total,
    "range: %s -> %s" % (first, last),
    "unresolved: %d" % unresolved,
    "top subsystems: %s" % procs.most_common(10),
    "boots: %s" % boots.most_common(6),
]
if pattern:
    lines.append("matches for %r: %d" % (pattern, len(hits)))
    for t, sub, cat, m in hits[:200]:
        lines.append("%s | %s | %s | %s" % (t, sub, cat, m[:300].replace("\n", " ")))
report = "\n".join(lines)
open(out + "/summary.txt", "w").write(report + "\n")
print(report)
PY

echo "[*] Wrote $OUT/unified.jsonl and $OUT/summary.txt"
