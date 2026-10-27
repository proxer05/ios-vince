#!/usr/bin/env python3
"""Build the daisy boot shim and, if a payload is present, pack daisy-boot.img.

For the full pipeline (ADT + payload + shim + boot image), use
`tools/build_daisy.sh KERNEL [OUTDIR]` instead.
"""
import os
import shutil
import subprocess
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM_DIR = os.path.join(BASE_DIR, "shim")
OUT_DIR = os.path.join(SHIM_DIR, "out")
ARTIFACTS_DIR = os.path.join(BASE_DIR, "artifacts")
TOOLS_DIR = os.path.join(BASE_DIR, "tools")

os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(ARTIFACTS_DIR, exist_ok=True)


def find_tool(*names):
    """Return the first tool found on PATH, else the first name (for a clear error)."""
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return names[0]


def run(cmd):
    print("    $ " + " ".join(cmd))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {cmd[0]}")


def build_shim():
    clang = find_tool("clang")
    ld = find_tool("ld.lld", "aarch64-linux-gnu-ld")
    objcopy = find_tool("llvm-objcopy", "aarch64-linux-gnu-objcopy")
    nm = find_tool("llvm-nm", "aarch64-linux-gnu-nm")

    print("[*] Building daisy boot shim")
    print(f"    Compiler: {clang}")
    print(f"    Linker:   {ld}")
    print(f"    Objcopy:  {objcopy}")
    print(f"    NM:       {nm}")

    cflags = [
        "-target", "aarch64-none-elf",
        "-O2", "-ffreestanding", "-nostdlib",
        "-fno-pic", "-mgeneral-regs-only", "-mstrict-align",
        "-fno-stack-protector", "-Wall", "-Wextra",
        "-I", SHIM_DIR,
    ]

    sources = ["start.S", "shim.c", "console.c", "mmu.c", "fdt.c", "loader.c"]
    objs = []
    for src in sources:
        src_path = os.path.join(SHIM_DIR, src)
        obj_path = os.path.join(OUT_DIR, os.path.splitext(src)[0] + ".o")
        objs.append(obj_path)
        print(f"[*] Compiling {src}")
        run([clang] + cflags + ["-c", src_path, "-o", obj_path])

    elf_path = os.path.join(OUT_DIR, "shim.elf")
    print("[*] Linking shim.elf")
    run([ld, "-T", os.path.join(SHIM_DIR, "shim.ld"), "--build-id=none", "-N",
         "-o", elf_path] + objs)

    bin_path = os.path.join(OUT_DIR, "Image")
    print("[*] Generating raw Image")
    run([objcopy, "-O", "binary", elf_path, bin_path])

    # The ARM64 Linux Image header advertises _end (including BSS and stacks);
    # pad the file so the loader's image size matches what the shim assumes.
    try:
        nm_out = subprocess.check_output([nm, elf_path], text=True)
        end_size = None
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_end":
                end_size = int(parts[0], 16)
                break
        if end_size:
            with open(bin_path, "a+b") as f:
                cur_len = f.tell()
                if end_size > cur_len:
                    f.write(b"\0" * (end_size - cur_len))
            print(f"    [+] Padded to _end (0x{end_size:x} bytes)")
    except Exception as exc:
        print(f"    [!] Warning during padding: {exc}")

    final_shim = os.path.join(ARTIFACTS_DIR, "daisy-shim.bin")
    shutil.copyfile(bin_path, final_shim)
    print(f"[+] Shim: {final_shim} ({os.path.getsize(final_shim)} bytes)")

    # Optional: append a live Qualcomm DTB if one was captured from the device.
    dtb_path = os.path.join(BASE_DIR, "research", "device_dumps", "live_fdt.dtb")
    kernel_to_pack = final_shim
    if os.path.exists(dtb_path):
        shim_dtb_path = os.path.join(ARTIFACTS_DIR, "daisy-shim-dtb.bin")
        with open(final_shim, "rb") as f_shim, open(dtb_path, "rb") as f_dtb, \
                open(shim_dtb_path, "wb") as f_out:
            f_out.write(f_shim.read())
            f_out.write(f_dtb.read())
        kernel_to_pack = shim_dtb_path
        print(f"[+] Appended hardware DTB: {shim_dtb_path}")

    return kernel_to_pack


def pack_boot_image(kernel_to_pack):
    payload_path = os.path.join(ARTIFACTS_DIR, "daisy.payload")
    if not os.path.exists(payload_path):
        print("[!] No artifacts/daisy.payload; shim built but no boot image packed.")
        print("    Run tools/build_daisy.sh KERNEL [OUTDIR] to build the full image.")
        return None

    sys.path.insert(0, TOOLS_DIR)
    import mkbootimg_vince

    bootimg_path = os.path.join(ARTIFACTS_DIR, "daisy-boot.img")
    print("[*] Packing daisy-boot.img")
    sha = mkbootimg_vince.pack_bootimg(
        kernel_path=kernel_to_pack,
        ramdisk_path=payload_path,
        out_path=bootimg_path,
        cmdline="console=null loop.max_part=7",
        page_size=2048,
    )
    print(f"[SUCCESS] {bootimg_path} (SHA256: {sha})")
    return bootimg_path


def main():
    try:
        kernel_to_pack = build_shim()
        pack_boot_image(kernel_to_pack)
    except RuntimeError as exc:
        print(f"[!] {exc}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
