#!/usr/bin/env python3
"""Corruption and capture-completeness checks for raw FT5446 evidence."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_daisy_reports import BEGIN, TRACE, analyze, decode_report, summarize_issues


IDLE = bytes.fromhex("000000" + "ff" * 60)
# DOWN slot0 (100,200); then CONTACT slot0 (120,240) + DOWN slot9 (1080,2280).
DOWN = bytes.fromhex("000001006400c83f90" + "ff" * 54)
MULTI = bytes.fromhex("000002807800f03f90043898e83f90" + "ff" * 48)
UP = bytes.fromhex("000000407800f03f90443898e83f90" + "ff" * 48)
GATES = "\n".join(("DAISY RPM: power result 0", "DAISY TOUCH: RPM r0 rails-ready1",
                    "DAISY TOUCH: GPIO id r0 reg9f stage7 id5422 tries1",
                    "DAISY TOUCH: GPIO FT5446 fw r0:0c vendor r0:51; probe only"))


def capture(frames=(IDLE, DOWN, MULTI, UP, IDLE)):
    lines = [GATES, "DAISY REPORT: " + BEGIN]
    lines.extend(f"DAISY REPORT: seq{i * 2 + 1} ms{i * 30} irq1/0 got63 data{frame.hex()}"
                 for i, frame in enumerate(frames))
    lines.append(f"DAISY REPORT: done ms300005 reads10000 ok10000 errors0 raw{len(frames)} reasontime")
    return "\n".join(lines)


TIMING = " us7000 phase500/900/1400/2000/6900 gap200 lowaaaaaaaaaaaaaaaa"


def traced_capture(frames=(IDLE, DOWN, MULTI, UP, IDLE)):
    lines = capture(frames).splitlines()
    result = []
    for line in lines:
        if line.startswith("DAISY REPORT: seq"):
            line += TIMING
        if line.startswith("DAISY REPORT: done"):
            result.append(f"DAISY REPORT: progress ms300004 reads10000 ok10000 errors0 raw{len(frames)} us7000/7000/7000 gap200")
        result.append(line)
        if line == "DAISY REPORT: " + BEGIN:
            result.append("DAISY REPORT: " + TRACE)
    return "\n".join(result)


class Reports(unittest.TestCase):
    def test_observed_0192_count_transition(self):
        # Exact seq497/498 from console-trace-0192-touch.log: header lagging the
        # appearance of slot1 DOWN, then a count-consistent CONTACT frame.
        bad = bytes.fromhex("000001813204b57fa0036214537fc0" + "ff" * 48)
        following = bytes.fromhex("000002813204b57fa0836214537f90" + "ff" * 48)
        with self.assertRaisesRegex(ValueError, "header count 1 != active records 2"):
            decode_report(bad)
        self.assertEqual([(c.slot, c.event, c.x, c.y) for c in decode_report(following)],
                         [(0, 2, 306, 1205), (1, 2, 866, 1107)])
        self.assertEqual(analyze(traced_capture((IDLE, bad, following, UP)))["exit_code"], 1)

    def test_trace_metadata_and_categories(self):
        result = analyze(traced_capture())
        self.assertEqual(result["exit_code"], 0, result)
        timing = result["timing"]["valid"]
        self.assertEqual(timing["total_us"], (7000, 7000, 7000))
        self.assertEqual(timing["phase_median_us"], (500, 900, 1400, 2000, 6900))
        self.assertEqual((timing["n"], timing["irq_change"], timing["p1_irq_change"]), (5, 5, 5))
        self.assertEqual(result["progress"]["raw"], 5)
        startup = b"\0" + b"\xff" * 62
        reserved = bytes.fromhex("0000028229061e7f70c00016173030" + "ff" * 48)
        result = analyze(traced_capture((startup, DOWN, reserved, UP)))
        self.assertEqual(result["exit_code"], 1)
        self.assertEqual({k: t["n"] for k, t in result["timing"].items()},
                         {"startup_ff": 1, "reserved_event": 1, "valid": 2})
        # A malformed sample breaks continuity, so it cannot prove a release.
        self.assertEqual(result["samples"]["full_release"], 0)

    def test_trace_corruption_rejected(self):
        text = traced_capture()
        for broken in (text.replace(TIMING, "", 1),
                       text.replace(TRACE, TRACE + "\nDAISY REPORT: " + TRACE),
                       text.replace("DAISY REPORT: " + TRACE + "\n", ""),
                       text.replace("phase500/900", "phase900/500", 1),
                       text.replace("gap200 low", "gap10 low", 1),
                       text.replace("us7000 phase", "us1000 phase", 1),
                       text.replace("lowaaaaaaaaaaaaaaaa", "lowaaaa", 1),
                       text.replace("raw5 us", "raw4 us"),
                       text.replace("us7000/7000/7000", "us8000/8000/8000"),
                       text.replace("us7000/7000/7000", "us6000/6000/6000"),
                       text.replace("gap200\nDAISY REPORT: done", "gap100\nDAISY REPORT: done"),
                       text.replace("done ms300005 reads10000 ok10000", "done ms300005 reads10001 ok10001")):
            with self.subTest(broken=broken[-200:]):
                self.assertEqual(analyze(broken)["exit_code"], 1)
        partial = analyze(text.rsplit("\n", 1)[0])
        self.assertEqual(partial["exit_code"], 2)
        self.assertIsNotNone(partial["progress"])

    def test_observed_0191_reports(self):
        # Exact device bytes from console-reports-0191.log (SHA-256 in touch plan).
        startup = bytes.fromhex("00" + "ff" * 62)  # seq1..1488, before first contact
        first = bytes.fromhex("00000101f8170c473041f4070c0000" + "ff" * 48)  # seq1498
        multi = bytes.fromhex("00000281fa170d5080837e07110000" + "ff" * 48)  # seq1500
        up = bytes.fromhex("000000420c17130000" + "ff" * 54)  # seq1504
        reserved = bytes.fromhex("0000028229061e7f70c00016173030" + "ff" * 48)  # seq1662
        self.assertEqual([(c.slot, c.event, c.x, c.y) for c in decode_report(first)],
                         [(1, 0, 504, 1804), (0, 1, 500, 1804)])
        self.assertEqual([(c.slot, c.event, c.x, c.y) for c in decode_report(multi)],
                         [(1, 2, 506, 1805), (0, 2, 894, 1809)])
        self.assertEqual([c.event for c in decode_report(up)], [1])
        with self.assertRaisesRegex(ValueError, "header count 15"):
            decode_report(startup)
        with self.assertRaisesRegex(ValueError, "reserved event for slot 1"):
            decode_report(reserved)
        # A plausible second-contact count must not make event 3 acceptable.
        result = analyze(capture((startup,) * 84 + (first, multi, reserved, up)))
        self.assertEqual(result["exit_code"], 1)
        groups = summarize_issues(result["issues"])
        self.assertEqual(len(groups), 2)
        self.assertIn("84 occurrence(s)", groups[0])
        self.assertIn("reserved event for slot 1", groups[1])

    def test_contacts_and_releases(self):
        self.assertEqual(decode_report(IDLE), [])
        self.assertEqual([(c.slot, c.event, c.x, c.y) for c in decode_report(MULTI)],
                         [(0, 2, 120, 240), (9, 0, 1080, 2280)])
        self.assertEqual([c.event for c in decode_report(UP)], [1, 1])
        result = analyze(capture())
        self.assertEqual(result["exit_code"], 0, result)
        self.assertEqual(result["samples"]["full_release"], 1)
        self.assertEqual(result["samples"]["motion"], 1)
        self.assertEqual(result["slots"], [0, 9])

    def test_coexisting_up_and_active(self):
        data = bytearray(MULTI)
        data[2], data[3] = 1, 0x40
        self.assertEqual([c.event for c in decode_report(data)], [1, 0])

    def test_corruptions_rejected(self):
        for index, value in ((0, 0x40), (2, 11), (2, 1), (3, 0xc0), (11, 8),
                             (3, 0x8f), (5, 0x0f)):
            data = bytearray(MULTI)
            data[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                decode_report(data)
        for data in (IDLE[:-1], IDLE + b"\0", bytes(63), b"\xff" * 63):
            with self.assertRaises(ValueError):
                decode_report(data)
        bad = analyze(capture((IDLE, bytes(63), MULTI, UP)))
        self.assertEqual(bad["exit_code"], 1)

    def test_incomplete_evidence(self):
        self.assertEqual(analyze(capture((IDLE,)))['exit_code'], 2)
        self.assertEqual(analyze(capture().rsplit("\n", 1)[0])['exit_code'], 2)
        self.assertEqual(analyze(capture().replace(GATES, ""))['exit_code'], 2)
        self.assertEqual(analyze("")['exit_code'], 2)

    def test_log_corruption_and_transport_fault(self):
        text = capture()
        for broken in (text.replace("got63", "got62", 1),
                       text.replace("data000000", "data0000", 1),
                       text.replace("seq3", "seq1", 1),
                       text.replace("ms60", "ms1", 1),
                       text.replace("raw5", "raw6"),
                       text.replace("reasontime", "reasonidentity"),
                       text.replace("ms300005", "ms299000"),
                       text.replace("reasontime", "reasonlimit"),
                       text.replace("ok10000 errors0", "ok9999 errors1"),
                       text + "\nDAISY REPORT: " + BEGIN,
                       text + "\n# DAISY EMMCLOG lost 40 bytes",
                       text.replace("DAISY REPORT: done", "DAISY REPORT: error seq9999 ms299990 r-4 stage7 got63 consecutive1\nDAISY REPORT: done")):
            with self.subTest(broken=broken[-160:]):
                self.assertEqual(analyze(broken)["exit_code"], 1)


if __name__ == "__main__":
    unittest.main()
