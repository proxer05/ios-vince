#!/usr/bin/env python3
"""Exercise the actual 0187 RPM C with a bounded legacy-SMEM/SMD peer."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="daisy-rpm-", dir="/tmp/opencode") as tmp:
        work = Path(tmp)
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        subprocess.run([
            "git", "-C", str(work), "apply", "--include=bsd/dev/daisy_touch_rpm.c",
            str(ROOT / "xnu/patches/0187-daisy-touch-rpm-power.patch"),
        ], check=True)
        source = (work / "bsd/dev/daisy_touch_rpm.c").read_text()
        source = re.sub(r"^#include .*\n", "", source, flags=re.M)
        for name, body in (
            ("dr_rd", "return mock_rpm_rd(base, off);"),
            ("dr_wr", "mock_rpm_wr(base, off, value);"),
            ("dr_sleep", "(void)dr_wait; mock_rpm_sleep();"),
        ):
            source, count = re.subn(
                rf"({name}\([^\n]*\)\n\{{)\n.*?\n\}}",
                rf"\1\n\t{body}\n}}", source, count=1, flags=re.S,
            )
            assert count == 1, name
        c = work / "test.c"
        c.write_text("""#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
static uint32_t mock_rpm_rd(vm_offset_t, uint32_t);
static void mock_rpm_wr(vm_offset_t, uint32_t, uint32_t);
static void mock_rpm_sleep(void);
""" + source + "\n" + (TESTS / "daisy_touch_rpm.c").read_text())
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
