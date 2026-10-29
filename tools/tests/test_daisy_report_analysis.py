#!/usr/bin/env python3
"""Corruption and capture-completeness checks for raw FT5446 evidence."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_daisy_reports import BEGIN, analyze, decode_report


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


class Reports(unittest.TestCase):
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
