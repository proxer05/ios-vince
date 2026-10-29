# Daisy FT5446 touchscreen bring-up

## Hardware identified in TWRP — 2026-10-29

Target: Xiaomi Mi A2 Lite (`daisy`), 3 GB / 64 GB, serial `6790a29e0305`.
Temporarily booted `/smb/twrp-3.7.1_12-daisy-20260424-1114.img` with
`fastboot boot`; ADB shell access and the bound touchscreen driver were confirmed.

| Item | Observed value |
| --- | --- |
| Controller | **FocalTech FT5446**, running chip ID `0x5422` |
| Module | **Ofilm touch panel + CSOT LCD**, as reported by the recovery driver |
| Firmware / vendor | `0x0c` (12) / `0x51` |
| Lockdown information | `34423201d1533401` |
| Linux driver | `fts_ts`, device name `5446`, driver version `Focaltech V2.0 20170919` |
| Input device | `/dev/input/event1`, `fts_ts`, direct-touch device, slots 0–9 |
| Reported coordinate ranges | X: 0–1080; Y: 0–2280 |
| I²C | Bus 3, 7-bit address `0x38`, BLSP1 QUP3 at `0x078b7000` |
| Bus configuration | 400 kHz output, 19.2 MHz input clock, GPIO 10 SDA / 11 SCL |
| Reset | GPIO 64, active low |
| Interrupt | GPIO 65, falling edge, active pin configuration has a pull-up |
| Analog supply | PM8953 L10 (`pm8953_l10`), DT constraint 2.85 V |
| I²C supply | PM8953 L6 (`pm8953_l6`), DT constraint 1.8 V |

Evidence includes a successful `get ic information, chip id = 0x5422` and
`[Vendor]Ofilm(TP) + CSOT(LCD), [TP-IC]FT5446,[FW]Ver12` in the kernel log.
The live node is `/soc/i2c@78b7000/focaltech@38`, compatible `focaltech,5446`.
Its older `focaltech,name = "ft5435"` property is not the detected chip identity.
The recovery tree also contains a Goodix node, but the Goodix probe NACKed;
the bound, identified controller on this handset is FT5446.

Linux IRQ 280 is this recovery's dynamically allocated IRQ number; XNU should
use GPIO 65 and its TLMM routing. The physical display remains 1080×2280.
The reported module vendor does not establish a different DSI panel controller.

**Recovery behavior:** this TWRP kernel automatically erased and rewrote the
touch controller's application flash during boot, reporting a successful
checksum and firmware `0x0c`. Its log reports both the previous controller
version and bundled version as `0x0c`. This was performed by recovery's own
auto-upgrade worker. The identification commands read sysfs/procfs information.

Local evidence is saved under `artifacts/daisy-touch-identification/`:
`identity.txt`, `input-capabilities.txt`, `controller-registers.txt`,
`touch-kernel-log.txt`, full `dmesg.txt`, and `device-tree.dtb` / `.dts`.
Identification is complete; a deliberate corner/swipe event capture remains
useful for establishing orientation and endpoint behavior.

## Implementation reference and existing XNU support

Protocol reference: Xiaomi's
[daisy-o-oss FT5446 driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/6421aa5cdc56e9ef7c6329e0a05164503e1513ef/drivers/input/touchscreen/ft5446),
pinned at `6421aa5cdc56e9ef7c6329e0a05164503e1513ef`.
Relevant files are `focaltech_core.c`, `focaltech_core.h`,
`focaltech_common.h`, and `focaltech_i2c.c`; local copies and provenance are in
`artifacts/daisy-touch-identification/vendor-ft5446/`.
This is a protocol reference, not an assertion that it is the exact source of
the running recovery's Linux 4.9.337 Onion kernel.

- Patch `0099-vince-input.patch` supplies `bsd/dev/vince_input.c`: QUP3,
  clocks, TLMM, polling threads and the `sweet_hid_touch()` connection.
  It currently hardcodes Synaptics address `0x20` and RMI4/F12 packets.
- `SweetHID.cpp` already supports ten finger identities and iOS digitizer
  events. Patches `0099`, `0116`, and `0117` adapt its panel dimensions and
  framebuffer scaling. This provides the route into backboardd/SpringBoard.
- The current QUP helper handles separate START–STOP messages and limits
  reads to FIFO capacity (32 bytes on the existing path). The vendor driver
  uses a combined register-address write/read and a **63-byte report**.
  Both transport differences need explicit handling.

## Staged plan

### 1. Establish a repeatable controller probe

Patch `0184-daisy-ft5446-probe.patch` now extends `vince_input.c` after the
working 0183 baseline with explicit FT5446 selection and reused QUP/TLMM
helpers. Keep `MACHINE_CONFIG=VINCE`.

Implemented probe-stage options (hardware validation pending):

- `daisytouch=1`: select FT5446 input; absent/zero keeps Daisy touch disabled.
- `daisytouch_probe=1`: with `daisytouch=1`, repeat the probe summary after
  30 and 60 seconds to aid console observation.

For this stage, `daisytouch=1` always runs a probe only: identity and one
63-byte report per summary, with **no finger events**. Omit both options
to preserve the known-working no-touch baseline.

The selector must give one thread exclusive ownership of QUP3. Daisy test
cmdlines use `vincetouch=0 daisytouch=1`; the FT5446 selector must work with
the Synaptics source disabled.

Configure GPIO 10/11 for BLSP I²C3 and GPIO 65 as input/pull-up. Follow the
vendor reset sequence: GPIO 64 low for 20 ms, then high and wait 200 ms.
Check clocks and the L10/L6 power state across bootloader handoff. Use the
existing PMIC/RPM facilities if explicit rail enables are required; the DT
voltages above are constraints, not measured rail values. Start at 100 kHz
and move to 400 kHz once reads are stable.

Read application registers `0xa3` and `0x9f` (expected `0x54`, `0x22`),
firmware `0xa6`, and vendor `0xa8`, with bounded readiness retries after reset.
Use the controller's resident application firmware for bring-up.

**Pass condition:** repeatable identity reads after reset, including a cold
boot through fastboot, rather than only immediately after TWRP.
Emit one concise probe summary plus bounded failure diagnostics. Verify its
visibility in a diagnostic-console image before interactive UI testing;
the current VINCE kernel RAM log is not automatically persisted to userdata.

### 2. Read complete, coherent touch reports

Parameterize the QUP slave address and implement combined register reads
with repeated START. Extend the receive path to service a counted 63-byte
transfer across FIFO boundaries, using the QUP's supported block/FIFO
service mechanism. Validate counts, timeout handling, NACK recovery and
STOP/bus-idle completion. Removing the length guard alone is insufficient.

Read one report starting at register `0x00`: three header bytes followed by
ten six-byte contact records. Avoid assembling a frame from independent
live reads until the controller's snapshot behavior has been established.

For contact record `p = report + 3 + 6*i`:

- Active-contact count: `report[2] & 0x0f`.
- Event: `p[0] >> 6`; 0 = down, 1 = up, 2 = contact/move.
- ID: `p[2] >> 4`; valid slots are 0–9. The reference parser stops at ID ≥10.
- X: `((p[0] & 0x0f) << 8) | p[1]`.
- Y: `((p[2] & 0x0f) << 8) | p[3]`.
- Pressure: `p[4]`; area: `p[5] >> 4`.

Inspect records beyond the active count because they can contain release
events. Validate the whole frame before updating contact state. Reconcile
missing contacts on valid frames and release contacts after bounded sustained
read failures or reset. Reject malformed counts, duplicate IDs, reserved
event values and incomplete transfers.

Start with an approximately 8 ms polling interval to establish functionality;
use GPIO 65 as diagnostic information initially. Its falling-edge interrupt
can be a pulse, so low-level-only sampling could miss events. Add interrupt
delivery and idle-rate tuning once report handling is proven.

**Pass condition:** stable raw tap, drag and lift sequences; all contacts
release correctly. Exercise the decoder with zero-contact/release frames,
ID reuse, multiple fingers, malformed packets and simulated read failures.

### 3. Connect touch to the visible iOS UI

Feed validated panel coordinates and stable slot IDs into `sweet_hid_touch()`.
Check one-finger operation first, then two-finger gestures and ten-slot
tracking. Audit frame batching in `SweetHID.cpp` so multitouch collections
represent coherent hardware frames rather than partially updated slots.

Match the existing framebuffer transform exactly. The normal 1125×2436
iPhone X surface fits into **1056×2280**, starting at physical column **12**.
The current HID scaling path already computes this for a 2280-high panel;
verify it on-device instead of independently scaling coordinates twice.
Also account for the existing fill/crop/native display options if selected.

Use the working artwork/ADT/root configuration with `VINCE_NO_FBCON=1` for
UI tests. Initially retain the Home-test fallback, then remove
`sweethidtest=30` for gesture validation so simulated Home presses cannot
interrupt an app or be mistaken for a successful swipe.

**Pass condition:** swipe up from the lockscreen, open an app, scroll, return
Home with a bottom-edge swipe, and accurately tap targets near all corners.
Verify orientation, edge flags, side-bar handling and lift events. Then
verify two-finger pinch/zoom and adding/removing fingers in different orders.

### 4. Verify lifecycle and preserve the working result

Test repeated lock/unlock, display sleep/wake, controller reset/recovery and
cold boots. Release active contacts before suspend/reset and restore the
controller's operating mode on resume. Measure polling CPU cost on the
current single-CPU configuration; use interrupt-driven reads for the mature
driver. Keep routine output to initialization summaries and rate-limited
errors, with raw frame logging opt-in and bounded.

Build XNU through the existing CI workflow with `MACHINE_CONFIG=VINCE`, check
that the new board patch was applied, and assemble each test under a new
artifact directory using `tools/build_daisy.sh`. Append the existing Android
DTB to the shim kernel and test with `fastboot boot`.

**Completion:** reliable touch-only homescreen entry, app interaction and
sleep/wake on this FT5446 handset. Record the kernel CI run, exact cmdline,
image hash and observed test results; publish the verified changes to
`daisy-boot` and synchronize `main`.

## Immediate next implementation

Stage 1 and the combined-register I²C transport are implemented. Host tests
run the patched C against a deterministic MMIO model, checking repeated-START
tags, 1/2/31/32/63/255-byte FIFO and block reads, buffer boundaries, NACK and
late-NACK handling, incomplete reads, bus-idle and input-done timeouts,
recovery, exclusive source selection, and the 20/200 ms reset sequence:

```sh
python3 tools/tests/test_daisy_touch_probe.py
```

These tests do not prove QUP behavior on-device. The first probe intentionally
does not change PMIC rails: identity reads will establish whether power is
retained across fastboot handoff, before adding any explicit enables.
Build the probe-only CI kernel and verify `0x5422` plus `report63 0` in XNU.
That establishes the power/reset/bus foundation before report decoding and
iOS gesture debugging. A CI marker check ensures the FT5446 probe was linked.

## First probe image — 2026-10-29

CI run [40896531839](https://github.com/proxer05/ios-vince/actions/runs/40896531839)
passed for commit `8a2c235cc0d673f8b8c56aeb44d3b167c9d91e5a`.
The log explicitly confirms application of `0184-daisy-ft5446-probe.patch`;
the linked kernel's Bluetooth and FT5446 marker checks passed.

- Kernel: `artifacts/kernel-0184/kernel.development.vince`.
- Kernel SHA-256: `35b7f211e070eec8289ad1b91feb5f4f75cfb024d27bf1e7ae766025571e2161`.
- Image: `artifacts/daisy-touch-probe-0184/daisy-boot.img`.
- Image SHA-256: `a4de08edd3b0f8eb85b6c64107c21f4eb5d21f32beaf9ed1f2d5803f8b541fa9`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0184/kernel.development.vince artifacts/daisy-touch-probe-0184
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-probe-0184/daisy-boot.img
```

This diagnostic image re-enables the framebuffer console to observe probe
results; it is not the no-console UI baseline. Startup phase and kprintf
output are suppressed to reduce noise, and automatic Home presses are omitted.
The packed image's artwork traits, RAM configuration, embedded ADT, appended
Android DTB and empty `second` field were verified. Phandle numbering shifts
when the console node is added; the artwork values are unchanged.

Fastboot reported successful transfer and boot. The user observed:

```text
DAISY TOUCH: probe failed -2 id 0000 (expected 5422)
DAISY TOUCH: check L10/L6 power and QUP3/pins before HID
```

`-2` is the driver's NACK result. `0000` is the initialized output buffer,
not a successfully read chip ID. The identity probe failed before the
63-byte report read; neither identity nor report transport is verified in XNU.
The hint about power/pins is diagnostic guidance, not a confirmed cause.
Expected successful summary:

```text
DAISY TOUCH: FT5446 id 5422 fw 0c vendor 51
DAISY TOUCH: report63 0 count ... irq ...; probe only
```

The summary repeats after 30 and 60 seconds. `report63 0` means transport
success, not validation of every contact record or proof of iOS touch input.

## Cross-check against the original Vince driver

Compared `0099-vince-input.patch` at its original commit `2c3b1c6` with the
Daisy source and the FT5446 vendor driver. The later Daisy edits to 0099
changed the default enable flag and panel height, not the QUP transport.
The separate `vince-bringup/shim/touch.c` cited in that patch is not present
in this checkout, so this comparison uses the actual committed XNU driver.

| Area | Vince XNU | Daisy FT5446 requirement / probe |
| --- | --- | --- |
| Controller | Synaptics TD4310, RMI4, `0x20` | FT5446, register protocol, `0x38` |
| QUP / pins | QUP3 `0x078b7000`, SDA 10, SCL 11, reset 64, attention 65 | Same wiring in recovery DT |
| Pinmux | GPIO 10/11 function 2, no internal pull | Function 2 matches MSM8953's `blsp_i2c3` mux table |
| Clock | 19.2 MHz source; 400 kHz with 100 kHz fallback | First probe uses Vince's 100 kHz divider |
| Register read | Register write with STOP, then a new read START; RMI page select as needed | Vendor Linux uses a combined write/read; 0184 implements repeated START |
| Reset | Drive GPIO 64 high, wait 200 ms | Vendor sequence is low 20 ms, high, wait 200 ms; 0184 follows it |
| Supplies | No touchscreen regulator enables in `vince_input.c` | Vendor enables `vdd`/L10 and `vcc_i2c`/L6 before reset; 0184 leaves rails unchanged |
| Reports | RMI F01/F12; four eight-byte records within FIFO limit | Three-byte header + ten six-byte records, 63-byte read |

**Conclusion:** matching MSM8953 wiring supports reusing Vince's bus setup,
but does not validate either the new repeated-START path or FT5446 power
retention after fastboot. The NACK cannot yet distinguish these possibilities.
Neither RMI page selection nor the Synaptics report decoder should be used
against FT5446.

Patch `0185-daisy-vince-touch-crosscheck.patch` adds an opt-in
`daisytouch_diag=1` comparison while retaining probe-only behavior:

- Use Vince's existing START–STOP helper at `0x38`, with no RMI page select.
- Compare STOP-separated and repeated-START chip-ID reads at 100/400 kHz.
- Show GPIO configuration/input levels and QUP3 core clock registers.
- Capture raw bus/QUP errors before the error flags are cleared.
- Restore the original I²C speed after the comparison; no PMIC, controller
  configuration, or firmware writes are added.

For a failing STOP read, `stage 1` is the `0xa3` register-selection write,
`stage 2` its read, `stage 3` the `0x9f` register-selection write, and
`stage 4` its read. The stage labels identify software transactions, not the
individual wire byte that received a NACK.

Host tests also check that Vince retains its `0x20` address when Daisy is
not selected, that the diagnostic STOP framing changes only the slave
address, that error snapshots survive cleanup, and that the original speed
is restored. **0185 device results are pending.**

## Vince-style comparison image — 2026-10-29

CI run [40906994679](https://github.com/proxer05/ios-vince/actions/runs/40906994679)
passed for commit `a7e07c0fe2a5ac5d32d1439f147138109f6dfb61` with
`MACHINE_CONFIG=VINCE`. The log confirms 0183, 0184 and 0185 were applied;
the linked Bluetooth, FT5446 and STOP/RESTART diagnostic checks passed.
The downloaded kernel's `SHA256SUMS` was verified.

- Kernel: `artifacts/kernel-0185/kernel.development.vince`.
- Kernel SHA-256: `d53770cf8093910feadd61f5a1d4fdf9a33319215dab432068f5a326f03a2518`.
- Image: `artifacts/daisy-touch-crosscheck-0185/daisy-boot.img`.
- Image SHA-256: `7d9dd163c264b8634290e4b6efc257a8f418e4ff7e8333f416cda7e7e4bc5d8e`.
- CI log: `artifacts/daisy-touch-dev/ci-build-0185.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_diag=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0185/kernel.development.vince artifacts/daisy-touch-crosscheck-0185
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-crosscheck-0185/daisy-boot.img
```

Verified the boot header, appended Android DTBs, empty `second` field,
embedded ADT, physical RAM base/size, 1080×2280 framebuffer console,
1125×2436 logical canvas, exact cmdline, and linked diagnostic markers.
The `/product` properties match the confirmed homescreen baseline (excluding
phandle numbering); the complete ADT matches 0184 except for its fresh random
seed. Host tests pass. No automatic Home presses, HID touch delivery, PMIC
writes, or controller firmware/configuration writes are enabled.

Fastboot reported successful transfer and temporary boot. **On-screen results
are awaiting user observation.** Photograph the `DAISY TOUCH:` pin/clock,
`STOP`, `RESTART`, `bus` and `qup` lines after 30–60 seconds. The comparison
repeats with the probe summaries; it is bounded to those summaries rather than
continuous logging. Identical NACKs on both methods would show the failure is
not specific to repeated START, but would not by themselves prove a missing
rail. Explicit rail enablement remains deferred pending this evidence.
