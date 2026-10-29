#!/usr/bin/env python3
"""Pull the 0190 XNU console file from Daisy's HFS root while in TWRP."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TSK = ROOT / "artifacts/tsk-build/sleuthkit-4.12.1/tools/fstools"
MAGIC = b"# DAISY EMMCLOG v1\n"
LIMIT = 32 * 1024 * 1024


def find_inode(listing, name, kind):
    matches = []
    for line in listing.splitlines():
        match = re.fullmatch(r"([dr])/\1 ([0-9]+):\t(.*)", line)
        if match and match[1] == kind and match[3] == name:
            matches.append(match[2])
    if len(matches) != 1:
        raise ValueError(f"expected one allocated {name!r}, found {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new local text file (never overwritten)")
    parser.add_argument("--serial", help="ADB device serial")
    parser.add_argument("--device", default="/dev/block/bootdevice/by-name/userdata",
                        help="recovery block-device path for the HFS root")
    parser.add_argument("--tsk-dir", type=Path, default=TSK,
                        help="directory with ARM64 fls-static and icat-static")
    args = parser.parse_args()
    adb = ["adb"] + (["-s", args.serial] if args.serial else [])

    def remote(*command, output=subprocess.PIPE):
        return subprocess.run(adb + ["exec-out", shlex.join(command)], stdout=output,
                              stderr=subprocess.PIPE, check=True, timeout=120)

    if args.output.exists():
        raise FileExistsError(f"output already exists: {args.output}")
    for name in ("fls-static", "icat-static"):
        if not (args.tsk_dir / name).is_file():
            raise FileNotFoundError(args.tsk_dir / name)
    if not remote("getprop", "ro.twrp.version").stdout.strip():
        raise RuntimeError("boot TWRP before extracting the HFS console file")
    remote("test", "-b", args.device)
    for name, target in (("fls-static", "/tmp/daisy-console-fls"),
                         ("icat-static", "/tmp/daisy-console-icat")):
        subprocess.run(adb + ["push", str(args.tsk_dir / name), target],
                       capture_output=True, check=True, timeout=120)
        remote("chmod", "755", target)

    # Walk allocated directory entries; never mount or write the filesystem.
    inode = "2"
    for name, kind in (("private", "d"), ("var", "d"), ("log", "d"),
                       ("daisy-console.log", "r")):
        listing = remote("/tmp/daisy-console-fls", "-f", "hfs", "-u",
                         args.device, inode).stdout.decode("utf-8", errors="replace")
        inode = find_inode(listing, name, kind)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".daisy-console-", dir=args.output.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            remote("/tmp/daisy-console-icat", "-f", "hfs", args.device, inode, output=out)
            out.flush()
            os.fsync(out.fileno())
        path = Path(temporary)
        size = path.stat().st_size
        if not len(MAGIC) <= size <= LIMIT:
            raise ValueError(f"unexpected console file size: {size}")
        data = path.read_bytes()
        if not data.startswith(MAGIC):
            raise ValueError("missing DAISY EMMCLOG header; extraction rejected")
        # Atomic publication without replacing any existing user output.
        os.link(path, args.output)
        print(f"Saved {size:,} bytes to {args.output}")
        print(f"SHA-256: {hashlib.sha256(data).hexdigest()}")
    finally:
        Path(temporary).unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or b"").decode("utf-8", errors="replace").strip()
        raise SystemExit(f"error: command failed ({error.returncode}): {detail}")
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"error: {error}")
