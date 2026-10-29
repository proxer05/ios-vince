# Daisy Adreno 506 bring-up

## Current status — 2026-10-29

The user considers touch sufficient for navigation and has asked to focus on
GPU acceleration. Preserve `artifacts/daisy-touch-hid-0194-smp/daisy-boot.img`
as the navigable baseline (SHA-256
`a6678e5576c3376c4424704c10d65aab7fbe13c7aca7546fe49cea12a9c75fc1`).

The imported `0181-vince-adreno-506-bringup.patch` contains hardware startup
steps and a `VinceGPU` IOKit command-submission service. It is compiled into
the current VINCE kernel, but hardware execution on Daisy is unvalidated.
Its `gpu=1` selector requests all eleven steps; `adreno=N` stops after step N.
Both are opt-in. A successful command-processor test would establish the
foundation for acceleration; SpringBoard still needs a working rendering path.

**First GPU diagnostic temporarily booted:**
`artifacts/daisy-gpu-regs-0194/daisy-boot.img`, with `adreno=6`. This checks
power, clocks, the TrustZone restore call and GPU register access. The
hardware result and its persistent log are pending.

## Recovery evidence

Temporarily booted TWRP on serial `6790a29e0305` after the user returned to
fastboot. Saved evidence under `artifacts/daisy-gpu-identification/`, with
`SHA256SUMS` covering the collected files:

- `kgsl.txt`: **Adreno506v1**, sampled frequency 320 MHz, seven advertised
  levels: 650, 560, 510, 400, 320, 216 and 133.33 MHz; reset count zero.
  These are Linux recovery observations, not XNU measurements.
- `device-tree.dtb`: SHA-256
  `8dbb42ad1be1ff4505108813f825edbf0c8b739a5b27cc1bc1590536e3e79afc`.
- `iomem.txt`, `dmesg.txt`, `cmdline.txt`, `mounts.txt`: live recovery layout
  and initialization evidence. `dmesg.txt` retains recovery's ANSI escapes.
- `firmware/` and `firmware-comparison.txt`: GPU firmware extracted from
  `vendor_a`, mounted **read-only with `noload`** at a temporary recovery path.
  `modem_a` was also inspected read-only. Both were unmounted before reboot.
  The HFS iOS root was not mounted during this inspection.

### GPU and memory layout

| Resource | Daisy evidence | Imported driver |
| --- | --- | --- |
| GPU registers | `0x01c00000`, size `0x40000` | Matches |
| GPU SMMU | `0x01c40000`, size `0x10000` | Assumes TZ leaves bypass; no page-table setup |
| PAS/ZAP subsystem | DT `qcom,pas-id=13`, `a506_zap` | Matches |
| GPU RAM scratch | `0x47800000..0x479fffff` lies in Linux System RAM and below XNU's RAM | Fixed 2 MiB region |
| ZAP reservation | DT uses an 8 MiB reusable pool, 4 MiB alignment, allocation range `0x80000000..0x8fffffff`; recovery allocates it at `0x8f800000` | Fixed `0x95000000`, size `0x2000` |

The recovery CMA allocation is not a fixed bootloader reservation. Neither
its address nor the inherited `0x95000000` should be described as an already
validated XNU ZAP destination. Establish a Daisy-owned region and check its
relationship to the bootloader, payload, framebuffer, XNU RAM and other
firmware before the ZAP execution test. The current `adreno=6` image stops
before GPU RAM/firmware setup.

XNU keeps `PHYS_BASE=0xa0000000`, `MEM_SIZE=0x57f00000`. The Android image
loads the shim at `0x80008000`, payload at `0x81000000`; the splash reservation
is `0x90001000..0x913fffff`. The collected recovery layout alone does not
prove GPU DMA accessibility or the claimed SMMU bypass state.

### Firmware comparison

| File | Daisy bytes | SHA-256 | Matches embedded 0181 |
| --- | ---: | --- | --- |
| `a506_zap.mdt` | 6,684 | `1498340d7474c4c0ae409452466c29f750e2e3a44afed606f4e8a79156b688dc` | Yes |
| `a506_zap.b02` | 5,188 | `fe8dead286927c662da0769589c739f914c130271429701606bd7662e91930fd` | Yes |
| `a530_pm4.fw` | 19,652 | `a02c9be11787472d4ab0173ed8552afbde1f31b1e81bcd87adf1fcea97b6115f` | Yes |
| `a530_pfp.fw` | 17,108 | `2a51c25b1f7299cb84c3685d3d1fb88bf0504494456345b2dfde32816e13bc8e` | **No** |

The embedded PFP is 16,820 bytes, SHA-256
`cc4f2b1cd6b3a9401592e2498ec43b87ba926a23848fa231f417ab08df9c47ba`.
The mismatch is established; it has not been shown to cause an XNU failure.
The first GPU diagnostic does not load either PFP image. Keep the native
PFP available for a separately identified command-execution build.

The native ZAP ELF has one relocatable `PT_LOAD` segment: physical address
`0x5000`, file/memory size `0x1444`, flags `0x08000007`. Metadata and the
segment match the embedded blobs. `b00`/`b01` are preserved alongside them.

## First test: steps 1–6 on the existing 0194 kernel

This reuses CI [40964379162](https://github.com/proxer05/ios-vince/actions/runs/40964379162)
and `MACHINE_CONFIG=VINCE`; no kernel changes were needed for the first test.
It retains filtered touch and the no-fbcon artwork ADT, uses `cpus=1` for
the persistent logger, and adds only `adreno=6` to the single-core HID image's
command line. No raw touch capture or delayed touch probes are selected.

```sh
VINCE_NO_FBCON=1 DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf daisylog=1 daisytouch_busywait=1 daisytouch_hid=1 adreno=6' \
tools/build_daisy.sh artifacts/kernel-0194/kernel.development.vince artifacts/daisy-gpu-regs-0194
fastboot -s 6790a29e0305 boot artifacts/daisy-gpu-regs-0194/daisy-boot.img
```

- Image: 17,944,576 bytes; SHA-256
  `a4a14e9aa41ceab14b6682635066d185a0087a2c1778ca69f9f2692680a2338e`.
- Kernel SHA-256:
  `010d54d08824cef3e5d28ae40ba553b36ae73469d319a26c1df4f024e8d71212`.
- Build log: `artifacts/daisy-gpu-identification/build-regs-image.log`.
- Verified Android header/hash, appended Daisy DTBs, empty `second`, full
  payload/kernel segments, ADT against the no-fbcon baseline, RAM bounds,
  command line, GPU service and touch/logger linked markers.
- Temporary boot handoff succeeded in **0.541 s**, after `product: daisy`.

Steps 1/2 read GCC and fuses; 3/4 power CX/GX; 5 programs the initial 100 MHz
clock selection and branch clocks; 6 calls `restore_sec_cfg(18,0)` and reads
GPU registers. The probe's stated expected VBIF version is `0x200d0000`.
This stage does not start `VinceGPU` or improve the rendering path.

When the phone returns to fastboot, temporarily boot recovery and extract
before another logging-enabled XNU boot:

```sh
fastboot -s 6790a29e0305 boot /smb/twrp-3.7.1_12-daisy-20260424-1114.img
adb -s 6790a29e0305 wait-for-recovery
python3 tools/pull_daisy_console.py artifacts/console-gpu-regs-0194.log --serial 6790a29e0305
```

Inspect every `adreno:`/`VINCE ADRENO:` result, clock readbacks, SCM return
and answer, VBIF version, subsequent boot progress and logger errors. The
current probe treats a completed register read as success, so the printed
`SUCCESS` lines alone are insufficient. Also, its step-6 `INT0` label reads
byte offset **`0xf8` instead of `0x1384`**; do not interpret that field as
the interrupt status. The other step-6 offsets match the pinned vendor header.
Later ring diagnostics use the correct interrupt-status dword `0x4e1`.

The GPU initializer is synchronous inside `vince_input_start()`. An early
abort may precede the filesystem logger; if no fresh log survives, move the
diagnostic behind a delayed logged worker for the next build.

## Remaining path to acceleration

1. **Register access:** collect the current hardware result. Correct the
   step-6 interrupt offset and make clock/SCM/readback failures explicit
   before relying on the startup gate for command execution.
2. **Command execution:** validate the chosen scratch/ZAP memory and SMMU
   state, then stage microcode/ring, `CP_ME_INIT`, scratch writes and ZAP
   authentication. Require the expected scratch values and consumed ring.
3. **Usable userspace buffers:** validate `VinceGPU` with multi-page buffers,
   repeated submissions, completion and close. Source inspection found that
   `gemNew()` uses the first physical address as a linear GPU address without
   requesting physical contiguity or establishing SMMU mappings. Buffer
   close releases memory immediately even with in-flight work, and the
   fence scratch word at `GPU_SCRATCH_OFF+0x20` is not initialized by step 8.
   Resolve these before connecting a renderer. Rendering completion also
   needs proper GPU/cache synchronization beyond a CP memory-write fence.
4. **Actual rendering:** integrate an Adreno-506-capable userspace renderer,
   shader compiler, IOSurface sharing and the CoreAnimation presentation
   path. The repository currently contains no Metal/OpenGL ES renderer
   implementation. `AppleCLCD` currently packs/copies the displayed surface
   into the 24-bpp panel buffer on the CPU.
5. **Interactive validation:** return to the eight-core, logging-disabled
   touch baseline with the validated GPU path enabled; verify real rendered
   output, navigation and measured frame timing.

### Pinned references

- Xiaomi vendor kernel `daisy-o-oss`, commit
  `6421aa5cdc56e9ef7c6329e0a05164503e1513ef`,
  [`drivers/gpu/msm/a5xx_reg.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/6421aa5cdc56e9ef7c6329e0a05164503e1513ef/drivers/gpu/msm/a5xx_reg.h).
  Local copy: `artifacts/daisy-gpu-identification/a5xx_reg.h`.
- Imported GPU code: `xnu/patches/0181-vince-adreno-506-bringup.patch`, through
  upstream `401873c2facdbb618c87ccc93ff479472884269a`.
- [`hakios-metal`](https://github.com/oosexgang/hakios-metal/tree/54a46fadec5bd9684ebd77f3a0834c84b5eb01a6),
  commit `54a46fadec5bd9684ebd77f3a0834c84b5eb01a6`: useful command-transport
  and blit reference. Its README explicitly says shaders and CoreAnimation
  through Metal are unfinished; it is not a ready-made SpringBoard renderer.
  The reported hardware checks are on Adreno 642L, not this Adreno 506.
