#!/usr/bin/env python3
"""Apply the input patches and run their actual C transport against mock MMIO.

Host tests do not prove QUP hardware behavior or XNU SDK compatibility.
Only the MMIO/barrier/sleep wrappers and kernel API declarations are replaced.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="daisy-touch-", dir="/tmp/opencode") as tmp:
        work = Path(tmp)
        path = work / "bsd/dev/vince_input.c"
        path.parent.mkdir(parents=True)
        lines = (ROOT / "xnu/patches/0099-vince-input.patch").read_text().splitlines()
        start = lines.index("@@ -0,0 +1,546 @@") + 1
        end = next(i for i in range(start, len(lines)) if lines[i].startswith("diff --git "))
        path.write_text("\n".join(line[1:] for line in lines[start:end]) + "\n")
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        for patch in (
            "0181-vince-adreno-506-bringup.patch", "0184-daisy-ft5446-probe.patch",
            "0185-daisy-vince-touch-crosscheck.patch",
            "0186-daisy-gpio-touch-diagnostic.patch",
            "0187-daisy-touch-rpm-power.patch",
            "0188-daisy-rpm-layout-diagnostic.patch",
            "0191-daisy-gpio-report-capture.patch",
            "0192-daisy-report-timing-irq.patch",
            "0193-daisy-gpio-counter-delay.patch",
            "0194-daisy-filtered-touch-hid.patch",
        ):
            subprocess.run([
                "git", "-C", str(work), "apply", "--include=bsd/dev/vince_input.c",
                str(ROOT / "xnu/patches" / patch),
            ], check=True)
        source = re.sub(r"^#include .*\n", "", path.read_text(), flags=re.M)
        for name, body in (
            ("in_rd", "return mock_rd(base, off);"),
            ("in_wr", "mock_wr(base, off, val);"),
            ("in_sleep", "(void)chan; mock_sleep(ms);"),
        ):
            source, count = re.subn(
                rf"({name}\([^\n]*\)\n\{{)\n.*?\n\}}",
                rf"\1\n\t{body}\n}}", source, count=1, flags=re.S,
            )
            assert count == 1, name
        c = work / "test.c"
        c.write_text((TESTS / "daisy_touch_stubs.h").read_text() + "\n" + source +
                     "\n" + (TESTS / "daisy_touch_gpio.c").read_text() +
                     "\n" + (TESTS / "daisy_touch_probe.c").read_text() +
                     "\n" + (TESTS / "daisy_touch_reports.c").read_text() +
                     "\n" + (TESTS / "daisy_touch_hid.c").read_text())
        binary = work / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=address,undefined", "-DARM64_BOARD_CONFIG_VINCE",
            str(c), "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE, text=True)
        for reason in ("reasontime", "reasonlimit", "reasoni2c", "reasonidentity"):
            assert reason in result.stdout, reason
        frames = re.findall(r"DAISY REPORT: seq\d+ ms\d+ irq[01]/[01] got63 data([0-9a-f]+)", result.stdout)
        assert frames and all(len(frame) == 126 for frame in frames)
        traces = re.findall(r" us(\d+) phase(\d+)/(\d+)/(\d+)/(\d+)/(\d+) gap(\d+) low([0-9a-f]{16})", result.stdout)
        assert traces
        for trace in traces:
            total, setup, header, p0, p1, data, gap = map(int, trace[:7])
            assert 0 < setup < header < p0 < p1 < data <= total
            assert 0 < gap <= data - setup
        assert "DAISY REPORT: progress " in result.stdout
        assert "DAISY TOUCH: delay5 legacy us25/25/25 samples32" in result.stdout
        assert "DAISY TOUCH: delay5 busy us5/5/5 samples32" in result.stdout
        print(result.stdout.splitlines()[-1])


if __name__ == "__main__":
    main()
