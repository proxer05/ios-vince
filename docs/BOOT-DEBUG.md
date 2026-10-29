# Daisy boot diagnostics

## Homescreen confirmed — 2026-10-29

The user confirmed the iOS homescreen on the Mi A2 Lite (3 GB / 64 GB)
after temporarily booting `artifacts/daisy-artwork-home-test/daisy-boot.img`.
This completes the first milestone: reach the homescreen without touch.
Touch remains intentionally disabled (`vincetouch=0`).

The preceding `artifacts/daisy-artwork-nofbcon/daisy-boot.img` boot reached
the visible lockscreen. Its SHA-256 was
`8443e2625ecad4d09fa1aa2e549243328c7a4134f92d2d8422d8bdbf78947ad0`.
The homescreen image adds `sweethidtest=30` to send simulated Home presses.

Working configuration:

- XNU: `artifacts/kernel-0183/kernel.development.vince`, CI run `40754164405`.
- Root: iPhone X iOS 16.7.16 (20H392), HFSX on GPT `userdata`, `rd=mmcroot0`.
- ADT: real iPhone X artwork/graphics traits under `/product`.
- `VINCE_NO_FBCON=1` omits the framebuffer serial console node so kernel
  messages do not overwrite the UI framebuffer.
- MobileGestalt's stale cache was invalidated before this boot.
- `sweethidtest=30` sends Home press/release events every 30 seconds, up to
  30 presses; this got past the lockscreen on the confirmed boot.
- Boot image SHA-256:
  `fc4f4603d2d8fe87207ec06cc0eb807ba396308804e7a45de5906fd96f2e1352`.

Reproduce the image assembly from the repository root:

```sh
VINCE_NO_FBCON=1 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 sweethidtest=30' \
tools/build_daisy.sh artifacts/kernel-0183/kernel.development.vince artifacts/daisy-artwork-home-test
fastboot -s 6790a29e0305 boot artifacts/daisy-artwork-home-test/daisy-boot.img
```

The generator uses a fresh random seed, so rebuilt image hashes differ.
The Android DTB is appended to the shim kernel; the boot image's `second`
field is empty.

### Bounding-path failure and the stale MobileGestalt cache

CoreUI reported that `MGCopyAnswer` returned only
`{ ArtworkDeviceScaleFactor = 3; }`, missing the idiom, gamut and graphics
feature class needed to select the bounding-path asset. The generator's
optional `research/ipsw-x/d22ap.adt` was absent, so its fallback `/product`
node lacked those traits. The ADT generator now supplies them explicitly.

Adding the properties alone did not resolve the existing root's failure.
The cache at
`/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`
still held the incomplete artwork dictionary. Its **source filesystem**
mtime was `2026-10-28 10:33:39 UTC`, preceding the artwork change. The later
timestamp initially cited during diagnosis belonged to the extracted copy,
not the source. Early-boot IOKit traces show the missing artwork lookups;
later boots use the cached answers without repeating those lookups.

For this particular snapshot, the 6927-byte plist occupied two 4096-byte
allocation blocks. Their contents were matched byte-for-byte to the extracted
plist before the corresponding 8192 bytes were zeroed in recovery. Readback
confirmed the invalidation. These extents are snapshot-specific, not reusable
offsets. The next artwork/no-fbcon boot reached the visible lockscreen; the
regenerated cache contents have not yet been collected.

### Log interpretation

The parser requires a flat logarchive layout: `Persist/`, `Special/`,
`Signpost/`, `HighVolume/`, `timesync/`, `dsc/`, and UUID prefix directories
at the archive root. Copying the on-device `private/var/db` hierarchy directly
produces unresolved shared strings.

Parsed timestamps overlap across boots and disagree with chunk mtimes. The
latest collected Persist chunks `22`–`25` belong to boot `D427D721...`, which
still had the pre-invalidation crash loop. Boot `424501B3...` is an older,
shorter record, not proof that the artwork fix had already succeeded.

### No-touch Home input

The current physical key mapping has no Home key or swipe gesture. The
existing HID service supports `sweethidtest=N`, sending a Home press/release
every N seconds for up to 30 presses. The confirmed homescreen image uses
`sweethidtest=30`. Local checks verified that this option and a fresh ADT random
seed were the only changes from the working lockscreen payload.

## Recovery log collection — 2026-10-28

Temporary recovery boot succeeded with:

```
fastboot -s 6790a29e0305 boot /smb/twrp-3.7.1_12-daisy-20260424-1114.img
```

The root filesystem is HFSX at the start of `/dev/block/mmcblk0p60`
(`userdata`): 4,194,304 allocation blocks of 4096 bytes = 16 GiB.
Recovery lacks an HFS+ driver. A read-only ADB copy was mounted read-only on
the host to recover files; no phone partitions were flashed or modified.
Redirect remote `dd` stderr to `/dev/null` when using `adb exec-out`, since
its status output otherwise appends to the raw image.

Recovered files are under local `artifacts/diagnostics-0183/` (not tracked).
The snapshot is `artifacts/daisy-userdata-0183.img`.
Useful filesystem paths:

- `/private/var/mobile/Library/Logs/CrashReporter/SpringBoard-*.ips`
- `/private/var/mobile/Library/Logs/CrashReporter/bluetoothd-*.ips`
- `/private/var/log/com.apple.xpc.launchd/launchd.log*`

**Correction:** VINCE does not use SWEET's persistent UFS console logger.
Patch 0092 defines a separate `vince_conlog` RAM ring (expanded to 8 MiB by 0177) in
`osfmk/console/serial_console.c`. SWEET's 4 MiB disk ring in `sweet_ufs.c`
does not flush this ring on `rd=mmcroot0`. Both userdata-tail reads lacked
`SWEETLOG`; pstore contained the previous recovery's Linux log, not XNU.

## SpringBoard failure confirmed by crash reports

All 27 recovered SpringBoard reports terminate with SIGABRT and include:

```
-[_UIScreenComplexBoundingPathUtilities _loadBitmapForScreen:type:]
-[_UIScreenComplexBoundingPathUtilities initWithScreen:]
+[_UIScreenBoundingPathUtilities boundingPathUtilitiesForScreen:]
-[UIScreen _boundingPathUtilities]
-[UIWindow _boundingPath]
```

The console's uncaught exception names
`BoundingPathBitmap-1125x2436-375x812-3.00x`. UIKitCore disassembly confirms
this assertion is the nil-data result of the asset manager's `dataNamed:`
lookup, before bitmap deserialization. It is not evidence of a missing
FrontBoard workspace directory. The earlier workspace diagnosis was based
on an inaccurate console transcription.

`/System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle/Assets.car`
exists on the phone (101,768 bytes) and matches the original root image:
SHA-256 `0a53453f1e096f70ccb38618b0b8c32fa950f71dc3b559c4ce556825517aa04d`.
The requested logical asset name occurs in this catalog. The artwork traits
and stale-cache diagnosis above resolved this failure in the later boot;
the physical panel remains 1080x2280.

## Why the first 0183 image still ran bluetoothd

CI run `40675481986` succeeded but its log explicitly says:

```
Skipping optional patch .../0183-daisy-disable-bluetooth.patch:
error: patch failed: bsd/dev/vince_rootfix.c:137
```

Patch 0165 had added wifip2pd setup after the print that 0183 expected to
be immediately followed by the closing brace. The downloaded kernel lacks
both `vince-bt.x` and the bluetoothd stand-in diagnostic string. The phone's
launchd cache still points at `/usr/sbin/bluetoothd`, and `vince-bt.x` is absent.

The corrected 0183 hunk includes 0165's context. `tools/patch_dxb.py` now
treats `*daisy*` patches as critical, and CI verifies the stand-in path and
diagnostic string in the built kernel. Local replay of the rootfix patches
through 0183 passes; the recovered launchd cache has exactly one matching
20-character bluetoothd string with binary-plist prefix `5f 10 14`.

## Opt-in eMMC console logging (0190)

Patch `0190-daisy-emmc-console-log.patch` adds `daisylog=1`, requiring the
existing `cpus=1 mmcrw=1` boot configuration. After root mount, a kernel worker
replays the 8 MiB console RAM ring into `/private/var/log/daisy-console.log`
and follows new output. It captures kernel and user writes routed through the
VINCE console, including the early touch/RPM diagnostics still in the ring.

- Disabled by default; enable by adding `daisylog=1` to `PAYLOAD_CMDLINE`.
- File mode 0600; starts with `# DAISY EMMCLOG v1`. Each logging-enabled boot
  replaces the previous logger file. Extract it before another such boot.
- Copies at most 16 KiB under the console lock with interrupts masked; disk
  I/O runs after releasing both. The snapshot requires single-CPU operation.
- Synchronous writes; fsync after each bounded 1 MiB batch, following new
  output every second once caught up. Stops on I/O failure or a 32 MiB limit.
- Explicit markers report overwritten RAM bytes and the file-size limit.
- This needs a mounted, writable HFS root. A crash before mount cannot be
  saved; output since the last completed flush can be missing after reset.
  The existing MMC cache-sync ioctl is a no-op, so this is not a guarantee
  against sudden power loss.

No root-image rebuild is needed. After running the logging kernel for at
least 60 seconds, enter fastboot with the buttons and temporarily boot TWRP:

```sh
fastboot -s 6790a29e0305 boot /smb/twrp-3.7.1_12-daisy-20260424-1114.img
python3 tools/pull_daisy_console.py artifacts/console-0190.log --serial 6790a29e0305
```

The pull tool uses the existing ARM64 static SleuthKit binaries in
`artifacts/tsk-build/sleuthkit-4.12.1/tools/fstools`. It walks the HFS catalog
and streams only this file from `userdata` without mounting the filesystem.
An existing local output is never overwritten. `--device /dev/block/mmcblk0p60`
can select this handset's partition explicitly if the by-name link is absent.

GCC and Clang sanitizer tests (`tools/tests/test_daisy_log.py`) cover early
replay, wrap/loss, opt-in selectors, console-lock/I/O separation, existing-file
guards, short/failed writes, flush failures, and the size cap. Device persistence
and extraction still require a logging-enabled boot followed by recovery.

The first combined CI run, `40943366267` at `23f0bdc`, compiled the upstream
GPU sources but rejected the logger's undeclared `thread_terminate` call.
Added the same explicit `kern_return_t thread_terminate(thread_t)` declaration
used by `vince_input.c` and `vince_smd.c`, and corrected the host stub's return
type. This is a compile fix; the logging behavior is unchanged.

## Upstream GPU refresh accompanying 0190

Merged `arabaci0711-ux/ios-vince` through
`401873c2facdbb618c87ccc93ff479472884269a`: `f2feeae`, `f774439`, `c713cbe`,
and the build-trigger commit. Upstream extends 0181 with `VinceGPU` and its
IOUserClient, GEM-style buffers, submissions, fences, parameter queries, and
the physical-address compile fixes. The service starts after successful
Adreno step 11; the existing `gpu=1`/`adreno=` bring-up selectors control it.
This imports the upstream interface; Daisy GPU execution remains unvalidated.

## 0190 logging image: CI passed and temporary boot sent

CI run [40944810745](https://github.com/proxer05/ios-vince/actions/runs/40944810745)
passed at `0278296c21a5fae0ca0d9aaac2c82d941d705019`, using
`MACHINE_CONFIG=VINCE`. The log confirms 0190 applied, `daisy_log.o` and
`VinceGPU.cpo` compiled, and the linked Bluetooth/touch/RPM/logger/GPU markers
passed. The downloaded kernel matches CI's `SHA256SUMS`.

- Kernel: `artifacts/kernel-0190/kernel.development.vince`.
- Kernel SHA-256: `cf464faa3b28090c6c04591c1f74a914fdb086482f3bd9194d5b2f67676b2d94`.
- Image: `artifacts/daisy-console-0190/daisy-boot.img` (17,928,192 bytes).
- Image SHA-256: `358d15f028098ad5fa5477617f1c2a5f99fa3392172b5ad80db1a742a9521e76`.
- Logs: `artifacts/daisy-touch-dev/ci-build-0190.log` and
  `build-image-0190.log`; the first failed build is retained as
  `ci-build-0190-first-failed.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_rpm_diag=1 startup_debug=0 -disable_kprintf daisylog=1' \
tools/build_daisy.sh artifacts/kernel-0190/kernel.development.vince artifacts/daisy-console-0190
fastboot -s 6790a29e0305 boot artifacts/daisy-console-0190/daisy-boot.img
```

Validated the Android v0 header/hash, exact appended Daisy DTBs, empty `second`,
complete embedded ADT, every kernel segment, RAM bounds, exact cmdline, and
linked markers before sending the image. The ADT matches 0189 apart from its
random seed. The cmdline adds only `daisylog=1` to the prior layout-only boot;
touch remains in read-only RPM layout mode, with GPU bring-up unselected.

Fastboot on serial `6790a29e0305` reported successful transfer and temporary
boot in 0.549 s. No partition was flashed. **Filesystem persistence is still
awaiting device validation:** let this boot run for 90 seconds, return to
fastboot, temporarily boot TWRP, and pull the console file using the command
above. A successful boot transfer alone does not prove the logger opened or
flushed its file.

### 0190 recovery extraction: early persistence confirmed

Temporarily booted TWRP `3.7.1_12-0` on serial `6790a29e0305`; fastboot
reported success in 1.903 s. The new pull tool extracted the file directly
from the unmounted HFS `userdata` filesystem:

```sh
python3 tools/pull_daisy_console.py artifacts/console-0190.log --serial 6790a29e0305
```

- Recovered size: **24,817 bytes**, 354 lines.
- SHA-256: `69aa08b03c877e4d43add25118e86507e61c529b9507c025d555ef201d8bfe62`.
- Starts with the logger header followed by the Darwin kernel banner.
- Includes the MMC write self-test, root mount, logger startup, complete first
  RPM layout diagnostic, and subsequent launchd/userland output.
- Ends at `exec usermanagerd [--init]`, after launchd's `13:08:44.263557` line.
  No 30/60-second layout repeats, logger-stop, or RAM-loss marker are present.

The user clarified that this boot was deliberately rebooted early and that
normal boots reach the lockscreen. This explains the early cutoff and missing
30/60-second repeats; the capture is not evidence of a boot or logger hang.
Early-buffer replay, persistence across the reboot to recovery, and the direct
single-file extraction path are validated. A future diagnostic boot should run
for at least 90 seconds to capture the scheduled repeats.

Source audit also identifies a retrieval limitation: journaled HFS's ordinary
`hfs_fsync()` updates the catalog in a transaction, while `hfs_metasync()` is a
no-op when a journal exists (`hfs_vnops.c:3214–3236,3262–3264`); full fsync uses
`HFS_FLUSH_FULL`. SleuthKit reads the catalog without replaying pending HFS
journal transactions. This remains a consideration for future recovery
captures, rather than an established cause of this deliberately short log.

### 0190 lockscreen run: sustained logging confirmed

The user next reported reaching the lockscreen and returning to fastboot.
Before replacing the on-device log, temporarily booted TWRP and extracted
`artifacts/console-0190-lockscreen.log`: **174,453 bytes**, 1,945 lines,
SHA-256 `6b38ca78c5062cf934123c30fbc147fb415d98f8f3191d61479608bb193ca92b`.

This capture contains the initial layout pass, both scheduled 30/60-second
repeats, the 90-second process sampler, and later SpringBoard/backboardd
activity. All three snapshots agree (`state0/4 tx120/120 rx78/78 bounds1`,
stage 6); L10 remains disabled and L6 enabled/ready. There are no logger-stop
or RAM-loss markers. Sustained console persistence and recovery extraction
are now validated on the device for this diagnostic interval.

Returned from TWRP with `adb -s 6790a29e0305 reboot bootloader`, verified
`product: daisy`, then temporarily booted the opt-in RPM power test described
in [the touch plan](TOUCHSCREEN-PLAN.md#0190-powerack-test-with-persistent-logging).

### 0190 power-test log collected

After the power test, temporarily booted TWRP and saved
`artifacts/console-power-0190.log` (283,516 bytes, 3,084 lines;
SHA-256 `0d9ca1525bb5a4048e226c504209555d3c92f31f5e98059b53cf3446b627769f`).
It contains both successful RPM vote/ACK records, `rails-ready1`, and three
first-attempt FT5446 identity reads, plus later userspace and both process
sampler passes. See [the power-test result](TOUCHSCREEN-PLAN.md#0190-power-test-result-rpm-and-gpio-identity-validated)
for the exact hardware evidence. The complete log was collected without
copying or mounting the full HFS root; the device remains in TWRP.

### 0191 report-capture boot

CI `40952756300` successfully built the opt-in GPIO report capture at `38f3151`.
The checksum-verified kernel is in `artifacts/kernel-0191/`; the assembled image
is `artifacts/daisy-touch-reports-0191/daisy-boot.img` (17,928,192 bytes,
SHA-256 `11cb751fe6f9daa33efd51d7bbc8b7848495f16fb8036f7a493c4310a665489b`).
It retains the proven 0190 power/logging configuration and adds
`daisytouch_reports=1`. Boot structure, payload, ADT and linked diagnostics
passed verification; `fastboot boot` succeeded on `6790a29e0305` in 0.538 s.
See [the report test](TOUCHSCREEN-PLAN.md#0191-report-test-image-built-and-temporarily-booted)
for the complete build command and capture protocol. Report hardware evidence
is pending recovery extraction to `artifacts/console-reports-0191.log`.

### 0191 report log recovered

Saved `artifacts/console-reports-0191.log`: 91,441 bytes, 965 lines,
SHA-256 `61c5d3a5baf140b3e5452effa3180389b6d086739d22facfd573f33040ef1d60`.
It contains 293 full report lines through 111.067 seconds, stable RPM/rail/
identity checks, and live contact/motion/release data. The user confirmed
rebooting after about two minutes; the absent five-minute summary is expected
for that interval and does not show logger failure. No logger-stop/loss markers
appear. Five reserved-event contact records still block clean validation;
see [the report result](TOUCHSCREEN-PLAN.md#0191-result-live-contacts-captured-coherence-unresolved).
TWRP was booted temporarily and the phone remains there after extraction.

### 0192 timing/IRQ diagnostic boot

CI `40957268230` at `b419648` passed compilation and linked diagnostic checks.
The checksum-verified kernel in `artifacts/kernel-0192/` was assembled into
`artifacts/daisy-touch-trace-0192/daisy-boot.img` (17,928,192 bytes;
SHA-256 `a688ae1684d8c1ecc8e30661b22b58858e4f9879db67d3abcfd258158f284b18`).
It adds `daisytouch_trace=1` to the 0191 report/power/logging arguments.
Image structure, payload/ADT, RAM bounds, segments and linked markers passed
verification; temporary fastboot boot succeeded on `6790a29e0305` in 0.538 s.
See [the timing/IRQ test](TOUCHSCREEN-PLAN.md#0192-timingirq-image-built-and-temporarily-booted)
for the build command and steady/moving-contact capture instructions. The next
recovery output is `artifacts/console-trace-0192.log`; hardware results are pending.

### 0192 complete baseline saved; contact repeat booted

Recovered `artifacts/console-trace-0192.log` (412,236 bytes, 4,215 lines,
SHA-256 `58f0318dd662d81c3f683a5b1ce31b617ccff15ff787bf752a40875e82e4ebaf`).
The capture completed normally after 300.027 seconds: 5,406 successful reads,
zero transport errors, 292 identical no-contact/startup-sentinel frames.
The user clarified they were AFK and did not touch during this interval.
The disappearance of diagnostic lines is explained by the five-minute limit.
The full completion/progress records validate sustained logging for this run;
the saved frames do not validate contact reports.

After the user confirmed readiness, reverified and temporarily booted the same
0192 image again on `6790a29e0305` (fastboot success in 0.533 s). The repeat
requests touches during the first five minutes, including steady one-/two-finger
holds. Retrieve it as `artifacts/console-trace-0192-touch.log`; see
[the baseline result](TOUCHSCREEN-PLAN.md#0192-first-trace-complete-no-contact-interval).

### 0192 contact trace saved

Recovered `artifacts/console-trace-0192-touch.log` (102,948 bytes, 829 lines;
SHA-256 `0721efdaaf3dad659f33bced586689a4ad819ad0a3f2ed3631125a0c06976fc0`).
The user deliberately rebooted after performing the tests; the saved trace
ends at 41.518 seconds. It has 266 structurally valid frames, including 125
two-contact samples, and three malformed contact frames. The timing points
to generic XNU `delay()` as a candidate for the slow GPIO bit clock; see
[the contact analysis and 0193 comparison](TOUCHSCREEN-PLAN.md#0192-contact-trace-timing-suspect-identified).
The phone is in temporary TWRP after collection.

### 0193 local GPIO delay comparison boot

CI `40961297908` at `7aed128` passed the VINCE build and linked marker checks.
The verified kernel in `artifacts/kernel-0193/` was assembled into
`artifacts/daisy-touch-busywait-0193/daisy-boot.img` (17,928,192 bytes;
SHA-256 `29506ebc17b44c188a1ffd2da24f0ab4e8a9e8839eae5cbc1eaae71bf0fe62a9`).
It adds `daisytouch_busywait=1` to the 0192 trace/power/logger configuration.
Boot structure, payload/ADT, RAM/segments, command line and diagnostic markers
passed verification. After the user confirmed readiness, temporary fastboot
boot succeeded on `6790a29e0305` in 0.541 s. See
[the 0193 test](TOUCHSCREEN-PLAN.md#0193-counter-delay-image-built-and-temporarily-booted)
for the command and expected evidence. Next recovery output:
`artifacts/console-busywait-0193.log`.

### 0193 counter-delay hardware result saved

Temporarily booted TWRP in 1.776 s and extracted
`artifacts/console-busywait-0193.log` (98,461 bytes, 898 lines; SHA-256
`812fd5dc7ed4dcbeef93e493f2af4a948dcf9fbb3354417041434b9da17186d0`).
Requested 5 us delays measure 22/23/37 us min/mean/max using the generic path
and 5/5/5 us with local spinning. Median valid-sample transfers fall from
31.310 to 8.799 ms. The capture contains 227 structurally valid frames,
including 182 two-contact samples, but two reserved-event records and one
header/active-count mismatch remain. Power/identity checks pass; the last
progress record has 901 successful reads and zero errors. The saved trace
ends at 39.105 seconds without a completion summary. See
[the detailed comparison](TOUCHSCREEN-PLAN.md#0193-hardware-result-faster-reads-coherence-still-blocked).
Touch HID remains disabled; the phone is in temporary TWRP.

### 0194 filtered touch HID boot

At the user's request, `daisytouch_hid=1` now enables experimental delivery of
valid FT5446 reports to SweetHID, dropping entire malformed frames and
releasing stale contacts after 150 ms without a valid report. Host GPIO/HID
tests pass with GCC and Clang ASan/UBSan. CI `40964379162` at `b6d0c87` passed
the VINCE build and linked marker checks.

The checksum-verified kernel in `artifacts/kernel-0194/` was assembled into
`artifacts/daisy-touch-hid-0194/daisy-boot.img` (17,944,576 bytes; SHA-256
`8c016465633d449fa5efff04279cc96e741b8e2aff75f32ad9f9d1bf77e1304a`).
It uses the known-working artwork/no-fbcon ADT, persistent logging, the
validated GPIO power/identity/busy-wait path, and continuous filtered HID.
Boot structure, payload/ADT, RAM/segments, exact command line and linked
markers passed verification. Temporary fastboot boot succeeded on
`6790a29e0305` in **0.540 s**. The image runs touch continuously.

See [the HID build/boot](TOUCHSCREEN-PLAN.md#0194-hid-image-built-and-temporarily-booted)
for the exact command. User confirmation of icon taps, swipes and coordinate
alignment is pending. Next recovery output: `artifacts/console-hid-0194.log`.

### 0194 single-core HID log and eight-core comparison boot

The user reported severe lag. Saved `artifacts/console-hid-0194.log`
(325,735 bytes, 3,743 lines; SHA-256
`fe3520b2a8ea0983df7246a368a6e05361225e3017d7ebcbe0cbde62694310c3`).
It confirms one CPU at 1401 MHz, 60 down/up pairs reaching SweetHID, and
continuous filtered HID through 390.458 seconds with no I²C errors or stale
releases. SpringBoard interaction remains unconfirmed.

Assembled `artifacts/daisy-touch-hid-0194-smp/daisy-boot.img` using the same
kernel with `DAISY_ALL_CPUS=1`, `cpus=8`, `daisylog=0` and no framebuffer
console. Daisy's local shim does not run the inherited CPU1 USB monitor, so
the optional ADT setting exposes all eight cores. The verified image is
17,944,576 bytes; SHA-256
`a6678e5576c3376c4424704c10d65aab7fbe13c7aca7546fe49cea12a9c75fc1`.
Temporary fastboot boot succeeded in 0.541 s. See
[the SMP comparison](TOUCHSCREEN-PLAN.md#0194-hid-delivery-confirmed-eight-core-lag-comparison)
for the full command and evidence. Runtime CPU count and responsiveness are
pending. Persistent logging is off; the previous on-device log is not evidence
from this new SMP boot.
