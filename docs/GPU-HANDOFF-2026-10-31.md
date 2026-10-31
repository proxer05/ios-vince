# Daisy GPU — end-of-day handoff, 2026-10-31

The user requested **log collection and preservation only, then stop for
today**. No further kernel changes or test boots were made after that
request. The phone was temporarily booted into TWRP for extraction; no
partition was flashed.

## Latest tested image

- `artifacts/daisy-gpu-pool-0199/daisy-boot.img`
- SHA-256: `aa71ae2f5be85a9889da0eceaef381769ed41c6f00107866554c9e781172e6bf`
- Kernel build: [41156231189](https://github.com/proxer05/ios-vince/actions/runs/41156231189),
  source commit `3d464f6`, `MACHINE_CONFIG=VINCE`.
- Arguments include `cpus=1 daisylog=1 adreno=11 daisygpu_test=1
  daisysurface_copy=1 daisygpu_pool=256`.
- User report: **still very laggy, clean blur, some missing shadows, much
  better visually**.

## Recovered console

`artifacts/console-gpu-pool-0199-20261031.log` is 589,992 bytes, SHA-256
`ac551b233bd697dd824fc86af180d25dfca0b67ac943fe4eb2f8416bd25631ec`.

- `GEM pool v1 reserved=256 MiB chunks=8 requested=256 MiB`.
- Allocator-backed memory test PASS, PA `0xa553f000`, completed fence 1.
- Explicit completed draw fences 2, 3, 4 and 7.
- Zero logged contiguous-allocation failures or pool-exhaustion messages.
- All twelve rate-limited wrapped-image diagnostics succeeded, including
  read-only static BGRA images that failed in 0198.
- One Mesa context and no console panic/backboardd crash markers.
- Two `glDrawElements: error 0x502` messages remain.
- At 600 frames: 268,800 us/frame cumulative average, 149,440 us/frame
  copying out, 11,202 us/frame making textures from IOSurfaces. No shared
  IOSurfaces; 264 had to be copied. These cumulative averages include
  startup/shader work and are not steady-state FPS measurements.
- 988 `can't perform kext scan: no kext summary` lines are also present;
  their associated backtraces have not yet been investigated.

## Unified logs and final device state

- Raw extraction and tarball: `artifacts/gpu-pool-0199-logs-20261031/`.
- Parsed log: `artifacts/gpu-pool-0199-logs-20261031/parsed/unified.jsonl`.
- Parsing completed with **1,123,348 messages, zero unresolved strings**.
- Filter boot UUID `08EEBA6266604C208A0E06157012A8B8` for **257,318 messages
  from the 0199 test**. The other three UUIDs belong to older tests.
- The phone, serial `6790a29e0305`, was left in **TWRP recovery**, with ADB
  available. Log collection is complete; no further work is running.

## Persistent copies of temporary work

Everything needed from the relevant `/tmp` work areas was copied into:

`artifacts/session-handoff-20261031/tmp/`

The directory preserves the original paths below `/tmp`, including:

- `opencode/daisy-0196-buildtree/`: full editable XNU/CI source tree, headers
  and its temporary Git repository. XNU HEAD is `34947ff` (0198 baseline);
  three tracked files contain the **already exported and deployed 0199**
  changes: `SweetSurface.h`, `SweetSurface.cpp`, `VinceGPU.cpp`. Treat these
  as the 0199 baseline before starting a new patch.
- `opencode/hakios-xnu-reference/`: reference checkout at
  `a3384e031eb1e605cf37adaefc19991f04f6274b`.
- `opencode/hakios-metal-reference/`: reference checkout at
  `54a46fadec5bd9684ebd77f3a0834c84b5eb01a6`.
- All other `opencode/` files, including packaging/verification scripts,
  GPU references, CI logs, extraction logs and the verified 0199 ZIP.
- `gpu-build-logs/`, `gpu-stack/`, `mesa-fd/`, `xnu-buildtree/`, `xnu-src/`,
  and `msm_drm.h` from the earlier work.

The snapshot includes the finalized 0199 pull/parse logs. Its
`COPY-VERIFICATION.txt` records byte-for-byte comparison against the source
files; `SHA256SUMS` records the persistent snapshot's file hashes. The
recovered-log directory has its own checksum manifest for the tarball and
parsed output.

Use the persistent source tree directly. Some saved helper scripts name
their old absolute `/tmp/opencode` paths; inspect those before rerunning.
The main repository's exported patches, tests and packaging tools are
already committed. No userdata rebuild is needed for the current kernel.

## Resume next session

1. Read the latest recovered logs and the relevant backtraces. The 0199
   allocator and read-only-image fixes have hardware evidence now.
2. Profile/remove the CPU frame-readback path through the existing
   shared-surface/GPU-blit integration, then test eight cores without
   persistent logging once the rendering path is stable.
3. Investigate remaining `0x502` errors and missing shadows using the
   Adreno 506/A5xx path. The imported framebuffer-fetch workaround in
   `userland/mesa/freedreno-darwin.patch` modifies A6xx, not A5xx; this is a
   review lead, not a confirmed cause of the shadows.

Keep `artifacts/daisy-touch-hid-0194-smp/daisy-boot.img` as the navigable
baseline, retain FT5446 touch behaviour, use `fastboot boot` only, keep
appended Daisy DTBs and empty `second`, and synchronize `daisy-boot`/`main`.
See `docs/GPU-PLAN.md` for the complete history and ABI/coherency constraints.
