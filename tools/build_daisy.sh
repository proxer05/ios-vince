#!/bin/sh
# Build a daisy (Xiaomi Mi A2 Lite) boot image from a VINCE XNU kernel.
#
#   tools/build_daisy.sh KERNEL [OUTDIR]
#
# KERNEL is the kernel.development.vince Mach-O produced by the ios-vince
# build (MACHINE_CONFIG=VINCE). OUTDIR defaults to ./artifacts and receives:
#   daisy.adt, daisy.payload, Image, shim.elf, daisy-boot.img
#
# The iOS root image is not packed here: XNU roots from the GPT "userdata"
# partition via rd=mmcroot0.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 KERNEL [OUTDIR]" >&2
    exit 2
fi

KERNEL=$1
OUT=${2:-"$ROOT/artifacts"}
mkdir -p "$OUT/obj"

CC=${CC:-clang}
LD=${LD:-}
if [ -z "$LD" ]; then
    LD=$(command -v ld.lld || command -v aarch64-linux-gnu-ld || true)
fi
OBJCOPY=${OBJCOPY:-}
if [ -z "$OBJCOPY" ]; then
    OBJCOPY=$(command -v llvm-objcopy || command -v aarch64-linux-gnu-objcopy || true)
fi
NM=${NM:-}
if [ -z "$NM" ]; then
    NM=$(command -v llvm-nm || command -v aarch64-linux-gnu-nm || true)
fi

if [ ! -f "$KERNEL" ]; then
    echo "error: kernel not found: $KERNEL" >&2
    exit 1
fi
if [ -z "$LD" ] || [ -z "$OBJCOPY" ] || [ -z "$NM" ]; then
    echo "error: need aarch64 ld, objcopy and nm (LLVM or GNU binutils)" >&2
    exit 1
fi

ADT="$OUT/daisy.adt"
PAYLOAD="$OUT/daisy.payload"
SHIM_ELF="$OUT/shim.elf"
SHIM_BIN="$OUT/Image"
BOOTIMG="$OUT/daisy-boot.img"

echo "[*] Generating 1080x2280 daisy ADT"
python3 "$ROOT/tools/mkadt_vince.py" "$ADT"

echo "[*] Compiling boot shim"
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic \
-mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -I $ROOT/shim"
for f in start.S shim.c console.c mmu.c fdt.c loader.c; do
    "$CC" $CFLAGS -c "$ROOT/shim/$f" -o "$OUT/obj/${f%.*}.o"
done

echo "[*] Linking shim"
"$LD" -T "$ROOT/shim/shim.ld" --build-id=none -N -o "$SHIM_ELF" \
    "$OUT/obj/start.o" "$OUT/obj/shim.o" "$OUT/obj/console.o" \
    "$OUT/obj/mmu.o" "$OUT/obj/fdt.o" "$OUT/obj/loader.o"
"$OBJCOPY" -O binary "$SHIM_ELF" "$SHIM_BIN"

# Pad to _end: the ARM64 Image header advertises the full image including
# BSS and the shim's stacks.
END=$("$NM" "$SHIM_ELF" | awk '$3 == "_end" { print $1; exit }')
if [ -n "$END" ]; then
    python3 - "$SHIM_BIN" "$END" <<'PY'
import os
import sys
path, end = sys.argv[1], int(sys.argv[2], 16)
size = os.path.getsize(path)
if end > size:
    with open(path, "ab") as f:
        f.write(b"\0" * (end - size))
print(f"    padded {path} to 0x{end:x} bytes")
PY
fi

echo "[*] Packing VINCEXNU payload"
PAYLOAD_CMDLINE=${PAYLOAD_CMDLINE:-"debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 vincetouch=0 vincekeys=1"}
python3 "$ROOT/tools/mkpayload_vince.py" "$KERNEL" "$ADT" "$PAYLOAD" \
    --cmdline "$PAYLOAD_CMDLINE"

echo "[*] Packing Android boot image"
python3 "$ROOT/tools/mkbootimg_vince.py" \
    --kernel "$SHIM_BIN" --ramdisk "$PAYLOAD" --out "$BOOTIMG" \
    --cmdline "console=null loop.max_part=7"

echo
echo "[SUCCESS] $BOOTIMG"
echo "    kernel:  $SHIM_BIN"
echo "    payload: $PAYLOAD"
echo "    cmdline: $PAYLOAD_CMDLINE"
echo "    test:    fastboot boot $BOOTIMG"
