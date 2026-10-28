# Daisy boot diagnostics — 2026-10-28

## Recovery log collection

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
Patch 0092 defines a separate 1 MiB `vince_conlog` RAM ring in
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
The requested logical asset name occurs in this catalog. Why the runtime
lookup returns no data remains to be diagnosed; changing the physical
1080x2280 panel dimensions is not yet justified.

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
