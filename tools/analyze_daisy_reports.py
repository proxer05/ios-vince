#!/usr/bin/env python3
"""Validate FT5446 raw captures and optional 0192 timing/IRQ observations.

Exit 0: complete, error-free capture with idle/motion/multitouch/release evidence.
Exit 1: malformed data or transport/capture failure. Exit 2: incomplete evidence.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import statistics


FRAME = re.compile(r"seq(\d+) ms(\d+) irq([01])/([01]) got63 data([0-9a-f]{126})"
                   r"(?: us(\d+) phase(\d+)/(\d+)/(\d+)/(\d+)/(\d+) gap(\d+) low([0-9a-f]{16}))?$")
DONE = re.compile(r"done ms(\d+) reads(\d+) ok(\d+) errors(\d+) raw(\d+) reason(\w+)$")
BEGIN = "begin reg00 bytes63 interval20ms duration300000ms limit4096; no HID"
TRACE = "trace v1 phases=addr,header,p0,p1,data low=before,byte1..63; polling unchanged"
PROGRESS = re.compile(r"progress ms(\d+) reads(\d+) ok(\d+) errors(\d+) raw(\d+) us(\d+)/(\d+)/(\d+) gap(\d+)$")


def decode_timing(match):
    if match[6] is None:
        return None
    total, setup, header, p0, p1, data, gap = map(int, match.groups()[5:12])
    if not 0 < setup < header < p0 < p1 < data <= total:
        raise ValueError("non-monotonic transfer phase times")
    if not 0 < gap <= data - setup or gap * 63 < data - setup:
        raise ValueError("byte gap inconsistent with data phase")
    return (total, setup, header, p0, p1, data, gap, int(match[13], 16))


def summarize_timing(rows):
    def spread(index):
        values = [r[index] for r in rows]
        return (min(values), statistics.median(values), max(values))
    def changed(mask, shift, bits):
        part = (mask >> shift) & ((1 << bits) - 1)
        return part not in (0, (1 << bits) - 1)
    return dict(n=len(rows), total_us=spread(0), gap_us=spread(6),
                phase_median_us=tuple(statistics.median(r[i] for r in rows) for i in range(1, 6)),
                irq_change=sum(changed(r[7], 0, 64) for r in rows),
                p1_irq_change=sum(changed(r[7], 9, 7) for r in rows))


@dataclass(frozen=True)
class Contact:
    slot: int
    event: int
    x: int
    y: int


def decode_report(data):
    """Vendor layout: header then ten records, terminated by first ID >= 10.

    UP records may coexist with active contacts; the header counts only DOWN/
    CONTACT records. Unused tail bytes are not contacts. Coordinates are raw
    Daisy panel units, before the existing HID transform.
    """
    if len(data) != 63:
        raise ValueError("report length is not 63")
    if data[0] & 0x70:
        raise ValueError(f"non-working mode {data[0]:02x}")
    count = data[2] & 15
    if count > 10:
        raise ValueError(f"header count {count} exceeds ten slots")
    contacts, seen = [], set()
    for offset in range(3, 63, 6):
        p = data[offset:offset + 6]
        slot, event = p[2] >> 4, p[0] >> 6
        if slot >= 10:
            break
        if slot in seen:
            raise ValueError(f"duplicate slot {slot}")
        seen.add(slot)
        if event == 3:
            raise ValueError(f"reserved event for slot {slot}")
        x, y = ((p[0] & 15) << 8) | p[1], ((p[2] & 15) << 8) | p[3]
        if x > 1080 or y > 2280:
            raise ValueError(f"slot {slot} outside panel: {x},{y}")
        contacts.append(Contact(slot, event, x, y))
    active = sum(c.event != 1 for c in contacts)
    if active != count:
        raise ValueError(f"header count {count} != active records {active}")
    return contacts


def analyze(text):
    issues, missing, samples = [], [], Counter()
    events, slots, gestures = Counter(), set(), Counter()
    previous, points = {}, []
    seq_last = ms_last = raw = error_lines = 0
    begun = ended = False
    summary = None
    traced, timings, progress = False, {}, None
    timing_min, timing_max, gap_max = None, 0, 0
    prefix = []
    for number, line in enumerate(text.splitlines(), 1):
        if "DAISY EMMCLOG: stopped" in line or "# DAISY EMMCLOG lost" in line:
            issues.append(f"line {number}: logger loss/stop marker")
        if "DAISY REPORT:" not in line:
            if not begun:
                prefix.append(line)
            continue
        record = line.split("DAISY REPORT: ", 1)[-1].strip()
        if record == BEGIN:
            if begun:
                issues.append(f"line {number}: repeated capture start")
            begun = True
            continue
        if not begun or ended:
            issues.append(f"line {number}: record outside capture")
        if record == TRACE:
            if traced or raw or error_lines:
                issues.append(f"line {number}: misplaced trace header")
            traced = True
            continue
        match = FRAME.fullmatch(record)
        if match:
            seq, ms, before, after = map(int, match.groups()[:4])
            raw += 1
            if seq <= seq_last or ms < ms_last or (progress and ms < progress["ms"]):
                issues.append(f"line {number}: non-monotonic sequence/time")
            seq_last, ms_last = seq, ms
            data = bytes.fromhex(match[5])
            timing = None
            if traced != (match[6] is not None):
                issues.append(f"line {number}: missing/unexpected byte timing")
            try:
                timing = decode_timing(match)
            except ValueError as exc:
                issues.append(f"line {number}, seq{seq}: {exc}")
            category = "valid"
            try:
                contacts = decode_report(data)
            except ValueError as exc:
                issues.append(f"line {number}, seq{seq}: {exc}")
                category = ("startup_ff" if data == b"\0" + b"\xff" * 62 else
                            "reserved_event" if str(exc).startswith("reserved event") else "other_invalid")
                previous = {}  # Do not infer motion/releases across a malformed report.
            if timing is not None:
                timings.setdefault(category, []).append(timing)
                timing_min = timing[0] if timing_min is None else min(timing_min, timing[0])
                timing_max, gap_max = max(timing_max, timing[0]), max(gap_max, timing[6])
            if category != "valid":
                continue
            samples["valid"] += 1
            samples["irq_low"] += int(not before or not after)
            gestures[data[1]] += 1
            active = {c.slot: (c.x, c.y) for c in contacts if c.event != 1}
            samples["idle"] += int(not active)
            samples["active"] += int(bool(active))
            samples["multitouch"] += int(len(active) >= 2)
            samples["motion"] += int(any(previous[s] != xy for s, xy in active.items() if s in previous))
            samples["full_release"] += int(bool(previous) and not active)
            samples["slot_releases"] += len(previous.keys() - active.keys())
            for c in contacts:
                events[c.event] += 1
                slots.add(c.slot)
            points.extend(active.values())
            previous = active
            continue
        match = PROGRESS.fullmatch(record)
        if match:
            p = dict(zip(("ms", "reads", "ok", "errors", "raw", "min_us", "mean_us", "max_us", "gap_us"),
                         map(int, match.groups())))
            if (not traced or not p["reads"] or p["ok"] + p["errors"] != p["reads"] or
                    p["raw"] != raw or p["raw"] > p["ok"] or p["reads"] < seq_last or
                    p["ms"] < ms_last or p["errors"] < error_lines or
                    not 0 <= p["min_us"] <= p["mean_us"] <= p["max_us"] or
                    p["gap_us"] > p["max_us"] or
                    (timing_min is not None and p["min_us"] > timing_min) or
                    p["max_us"] < timing_max or p["gap_us"] < gap_max or
                    (progress and any(p[k] < progress[k] for k in ("ms", "reads", "ok", "errors", "raw")))):
                issues.append(f"line {number}: inconsistent progress counters/timing")
            progress = p
            continue
        match = DONE.fullmatch(record)
        if match:
            summary = dict(zip(("ms", "reads", "ok", "errors", "raw"), map(int, match.groups()[:5])))
            summary["reason"] = match[6]
            ended = True
            continue
        if record.startswith("error "):
            error_lines += 1
            previous = {}
            issues.append(f"line {number}: {record}")
        else:
            issues.append(f"line {number}: malformed/blocked capture record")

    if not begun:
        missing.append("capture start")
    before_capture = "\n".join(prefix)
    for marker in ("DAISY RPM: power result 0", "DAISY TOUCH: RPM r0 rails-ready1",
                   "DAISY TOUCH: GPIO id r0 reg9f stage7 id5422",
                   "DAISY TOUCH: GPIO FT5446 fw r0:0c vendor r0:51; probe only"):
        if marker not in before_capture:
            missing.append(marker)
    if summary is None:
        missing.append("capture completion summary (run up to five minutes)")
    else:
        if (summary["ok"] + summary["errors"] != summary["reads"] or
                summary["raw"] != raw or summary["raw"] > summary["ok"] or
                summary["reads"] < seq_last or summary["ms"] < ms_last or
                summary["errors"] < error_lines or raw > 4096):
            issues.append("summary counters disagree with captured records")
        if summary["reason"] not in ("time", "limit") or summary["errors"]:
            issues.append(f"capture ended with reason={summary['reason']}, errors={summary['errors']}")
        if summary["reason"] == "time" and summary["ms"] < 300000:
            issues.append("time-bounded capture ended before five minutes")
        if summary["reason"] == "limit" and raw != 4096:
            issues.append("frame-limit completion without 4096 raw frames")
        if traced:
            if progress is None:
                missing.append("final trace progress")
            elif (any(summary[k] != progress[k] for k in ("reads", "ok", "errors", "raw")) or
                  summary["ms"] < progress["ms"]):
                issues.append("final progress disagrees with completion summary")
    for key in ("idle", "active", "motion", "multitouch", "full_release"):
        if not samples[key]:
            missing.append(key)
    bounds = None
    if points:
        xs, ys = zip(*points)
        bounds = (min(xs), max(xs), min(ys), max(ys))
    return dict(issues=issues, missing=missing, samples=dict(samples), events=dict(events),
                slots=sorted(slots), gestures=dict(gestures), bounds=bounds,
                raw=raw, summary=summary, progress=progress,
                timing={name: summarize_timing(rows) for name, rows in timings.items()},
                exit_code=1 if issues else 2 if missing else 0)


def summarize_issues(issues):
    """Keep repeated startup errors from hiding a different contact-frame fault."""
    counts, first = Counter(), {}
    for issue in issues:
        category = issue.split(": ", 1)[-1] if issue.startswith("line ") else issue
        counts[category] += 1
        first.setdefault(category, issue)
    return [f"{count} occurrence(s): {first[category]}" for category, count in counts.items()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    data = args.log.read_bytes()
    result = analyze(data.decode("utf-8", errors="replace"))
    print(f"{args.log}: {len(data):,} bytes; SHA-256 {hashlib.sha256(data).hexdigest()}")
    print(f"Raw frames: {result['raw']}; summary: {result['summary']}")
    print(f"Samples: {result['samples']}")
    print(f"Events (0=down, 1=up, 2=contact): {result['events']}; slots: {result['slots']}")
    print(f"Raw coordinate bounds (xmin,xmax,ymin,ymax): {result['bounds']}")
    if result["progress"]:
        print(f"Latest cumulative progress: {result['progress']}")
    for category, timing in result["timing"].items():
        print(f"Timing {category} (logged samples; min/median/max us): {timing}")
    groups = summarize_issues(result["issues"])
    for issue in groups[:20]:
        print(f"FAIL: {issue}")
    if len(groups) > 20:
        print(f"... {len(groups) - 20} further issue categories")
    for item in result["missing"]:
        print(f"MISSING: {item}")
    print(("VALIDATED: coherent sampled reports with idle, motion, multitouch and release evidence",
           "FAILED: inspect transport/structure evidence",
           "INCOMPLETE: additional capture evidence required")[result["exit_code"]])
    return result["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
