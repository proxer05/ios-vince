#!/usr/bin/env python3
"""Replay the black-screen IOSurface calls against the patched C++ and mock IOKit."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="daisy-surface-", dir="/tmp/opencode") as tmp:
        work = Path(tmp)
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        for number in ("0092", "0096", "0197", "0198", "0199"):
            patch, = (ROOT / "xnu/patches").glob(number + "-*.patch")
            subprocess.run([
                "git", "-C", str(work), "apply",
                "--include=iokit/Kernel/arm/SweetSurface.cpp",
                "--include=iokit/Kernel/arm/SweetSurface.h", str(patch),
            ], check=True)
        source = "\n".join((work / "iokit/Kernel/arm" / name).read_text()
                           for name in ("SweetSurface.h", "SweetSurface.cpp"))
        source = re.sub(r"^#include .*\n", "", source, flags=re.M)
        # Native host fence instead of ARM instructions; mapping/cache behaviour
        # and device execution still require the real VINCE build and phone.
        source = re.sub(r'__asm__ volatile\("(?:dsb sy|dmb ishst)" ::: "memory"\);',
                        'std::atomic_thread_fence(std::memory_order_seq_cst);', source)
        cpp = work / "test.cpp"
        cpp.write_text((TESTS / "daisy_surface_stubs.h").read_text() + "\n" + source +
                       "\n" + (TESTS / "daisy_surface.cpp").read_text())
        binary = work / "test"
        subprocess.run([
            os.environ.get("CXX", "clang++"), "-std=gnu++17", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-Wno-multichar", "-Wno-unused-parameter",
            "-Wno-c99-designator", "-fsanitize=address,undefined",
            "-DARM64_BOARD_CONFIG_VINCE", str(cpp), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
