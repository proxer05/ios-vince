#!/usr/bin/env python3
"""Stress the patched GEM reserve's allocations, mappings and reclamation."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="daisy-gpu-pool-", dir="/tmp/opencode") as tmp:
        work = Path(tmp)
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        for number in ("0181", "0195", "0196", "0197", "0199", "0200"):
            patch, = (ROOT / "xnu/patches").glob(number + "-*.patch")
            subprocess.run(["git", "-C", str(work), "apply",
                            "--include=iokit/Kernel/arm/VinceGPU.cpp", str(patch)], check=True)
        source = (work / "iokit/Kernel/arm/VinceGPU.cpp").read_text()
        # Exercise the real allocator and descriptor subclass, replacing only
        # IOKit/physical memory and ARM cache maintenance on the host.
        source = source[source.index("static IOBufferMemoryDescriptor *\ngpuAllocate"):
                        source.index("IOMemoryDescriptor *\nvince_surface_allocate")]
        cpp = work / "test.cpp"
        cpp.write_text((TESTS / "daisy_gpu_pool_stubs.h").read_text() + "\n" + source +
                       "\n" + (TESTS / "daisy_gpu_pool.cpp").read_text())
        binary = work / "test"
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=gnu++17", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
