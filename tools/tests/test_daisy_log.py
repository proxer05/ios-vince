#!/usr/bin/env python3
"""Exercise actual 0190 console snapshot and VFS logger with fault injection."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def main():
    patch = ROOT / "xnu/patches/0190-daisy-emmc-console-log.patch"
    with tempfile.TemporaryDirectory(prefix="daisy-log-", dir="/tmp/opencode") as tmp:
        work = Path(tmp)
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        subprocess.run([
            "git", "-C", str(work), "apply", "--include=bsd/dev/daisy_log.c", str(patch),
        ], check=True)
        source = (work / "bsd/dev/daisy_log.c").read_text()
        source = re.sub(r"^#include .*\n", "", source, flags=re.M)
        source, count = re.subn(
            r"(dl_sleep\(unsigned ms\)\n\{)\n.*?\n\}",
            r"\1\n\t(void)dl_wait; mock_sleep(ms);\n}", source, count=1, flags=re.S,
        )
        assert count == 1
        section = patch.read_text().split("diff --git a/osfmk/console/serial_console.c", 1)[1]
        console = "\n".join(line[1:] for line in section.splitlines()
                            if line.startswith("+") and not line.startswith("+++"))
        c = work / "test.c"
        c.write_text((TESTS / "daisy_log_stubs.h").read_text() + "\n" + console + "\n" +
                     source + "\n" + (TESTS / "daisy_log.c").read_text())
        binary = work / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=address,undefined", "-DARM64_BOARD_CONFIG_VINCE",
            str(c), "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        print(result.stdout.splitlines()[-1])


if __name__ == "__main__":
    main()
