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
                     "\n" + (TESTS / "daisy_touch_probe.c").read_text())
        binary = work / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=address,undefined", "-DARM64_BOARD_CONFIG_VINCE",
            str(c), "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE, text=True)
        print(result.stdout.splitlines()[-1])


if __name__ == "__main__":
    main()
