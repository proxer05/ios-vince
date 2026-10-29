# Daisy FT5446 touchscreen bring-up

**Current milestone (0190):** RPM L10/L6 votes and matching ACKs, enabled/ready
rail readback, and FT5446 GPIO identity (`0x5422`, firmware `0x0c`, vendor `0x51`)
are validated on this handset, including the 30/60-second identity repeats.
The power evidence is in `artifacts/console-power-0190.log`. The 0192 contact
trace contains live motion, two-contact and release samples, but one reserved
event and two count inconsistencies prevent a clean coherence result. A local
GPIO delay comparison is next; touch HID remains disabled.

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

Implemented QUP probe-stage options (QUP/report hardware validation pending):

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
visibility in a diagnostic-console image before interactive UI testing.
The 0190 `daisylog=1` worker now persists the VINCE console to the HFS root;
the successful identity path currently uses `daisytouch_gpio=1` with RPM power.

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

## GPIO/RPM reference review and independent probe (0186)

User-provided reference: [grinkodbogdan/rpiios](https://github.com/grinkodbogdan/rpiios/tree/15fc60d01eee053cecc8f43f0983c51e7bafc0fb),
branch `ccr-4261c3e6-0ebsg3`, pinned at
`15fc60d01eee053cecc8f43f0983c51e7bafc0fb`.
The relevant additions inside `hackios/hackios-xnu-kernel.diff` are
`redwood_tissot_touch.c`, `redwood_tissot_buttons.c`, and
`redwood_tissot_rpm.c`. Copies of the diff and the HackiOS/Mi A1 READMEs are
saved under `artifacts/daisy-touch-identification/rpiios-15fc60d/`.
Diff SHA-256: `a6232e3b335fe48dd0b11ab4ce3b4d6c66d3c44d41f17605d3adb8ee96fcdc2a`.

Applicable findings:

- Tissot uses the same FocalTech address `0x38` and GPIO10/11/64/65 wiring,
  with open-drain GPIO I²C instead of QUP. The FT contact layout is relevant,
  but its 1080×1920 geometry and `redwood_touch_post()` scaling are not Daisy's.
- The source requests L10 at 2.85 V and L5/L6 at 1.8 V through RPM/SMD before
  reset. The HackiOS README's claim of read-only supplies is stale; the Mi A1
  README reflects the newer code. Daisy's recovered FT5446 DT names L10 and
  L6 as its supplies; an L5 vote is not established by that node.
- Its RPM channel uses the legacy SMEM table, 44-byte word-flag halves and
  a different doorbell from Vince's current 20-byte Pronto channel helpers.
  Reusing those helpers unchanged would be incorrect. The reference also
  lacks request-ID validation in replies and TX free-space checking.
- The reference GPIO receive/STOP paths ignore clock-stretch failure. Its
  first contact count and records can come from separate reads; its decoder
  processes only `count` records and does not expire contacts after sustained
  read failures. These are not carried over to Daisy's planned live driver.
- The [last explicit touch hardware report](https://github.com/grinkodbogdan/rpiios/commit/4437e2a735d6ee1609a271c871bbbb2a364c6ffd)
  says touch did not work, before RPM support was added in `310cbd4`.
  Later commits examined describe the RPM implementation but do not confirm
  successful hardware touch. The reference is useful, not proof of transport.

Patch `0186-daisy-gpio-touch-diagnostic.patch` implements a new, opt-in path:

```text
vincetouch=0 daisytouch=1 daisytouch_gpio=1 daisytouch_probe=1
```

`daisytouch_gpio` alone never enables touch. With `daisytouch=1` it selects
one GPIO-only probe thread even if `vincetouch=1` is also supplied. It bypasses
QUP/GCC mapping and initialization and supersedes `daisytouch_diag`.

The probe reads inherited L10/L6 state through SPMI v2 observer channels
before reset, then uses the vendor 20 ms low / 200 ms high reset sequence.
It reads application ID `0xa3`/`0x9f` with up to three attempts, followed by
firmware/vendor on a matching `5422`. Optional summaries repeat twice at
30-second intervals. This path reads identity only; it does not replace the
QUP 63-byte transport, deliver HID events, or write controller configuration,
firmware, PMIC registers, or RPM requests.

GPIO transport drives only low and releases high as an input, using existing
external pull-ups. It propagates timeout errors from every SCL-high operation,
including receive, final NACK and STOP. Failed reads leave their output byte
unchanged and both lines released. A non-idle bus gets one bounded recovery
sequence of up to nine clocks and STOP per summary. SDA/SCL remain released
as plain GPIO inputs after the diagnostic completes; there is no QUP handoff.

The observer checks for arbiter v2, scans its 512 channel-map entries, and maps
only the L10/L6 observer pages (PPIDs `0x149` / `0x145`). It reads type/subtype
`0x04/05`, status `0x08`, enable `0x46`, and voltage selectors `0x40/41`.
Output `L10 p149 r0 type ... st... en... v.../...` is valid only for `r0`;
failed reads print an error and zero-initialized/partial fields. Raw selectors
are not a measured voltage, and status/enable require the appropriate PMIC
type interpretation. Mapping/version failures produce no PMIC commands.

GPIO read stages: `1` START/bus-free check, `2` write address, `3` register,
`4` repeated START, `5` read address, `6` data/final NACK, `7` STOP.
Errors: `-2` slave NACK, `-4` SCL-high timeout, `-6` SDA not released.
`id0000` on failure remains an initialized/partial value, not a valid chip ID.
GPIO success would point toward the QUP path or its setup; repeatability across
cold boots is still needed. GPIO failure plus raw rail state guides the next
power/reset investigation.

Host tests run the actual patched C against an edge-driven open-drain slave
model and observer MMIO. They check address/register bytes, repeated START,
final master NACK, SCL timeout at every transfer clock (including receive and
STOP), finite stretching, slave NACK at each transmit byte, stuck lines,
bounded recovery/retries, unpublished failed reads, released outputs, reset
timing, exclusive selection, no QUP accesses, and observer-only read commands.
Missing mappings, unsupported arbiter versions, APIDs above 255, denied reads
and read timeouts are exercised too. Host tests and CI pass; **0186 device
results are pending.**

### GPIO diagnostic image — 2026-10-29

CI run [40912830845](https://github.com/proxer05/ios-vince/actions/runs/40912830845)
passed for commit `a6c26c98b92978f8e1aa472a9addb73f7578b77e`, retaining
`MACHINE_CONFIG=VINCE`. The build log confirms 0186 applied, and the linked
Bluetooth, QUP, GPIO and rail diagnostic marker checks passed. The downloaded
kernel matches its CI `SHA256SUMS`.

- Kernel: `artifacts/kernel-0186/kernel.development.vince`.
- Kernel SHA-256: `7041f9f3ded1454d1b542df332c6b01509e13338c817074b188c7d315abd2227`.
- Image: `artifacts/daisy-touch-gpio-0186/daisy-boot.img` (17,901,568 bytes).
- Image SHA-256: `b9814158c8a2d87144a835ad10a664d24c3e55b4c897fb15a3711f74b277ce68`.
- Logs: `artifacts/daisy-touch-dev/ci-build-0186.log` and `build-image-0186.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0186/kernel.development.vince artifacts/daisy-touch-gpio-0186
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-gpio-0186/daisy-boot.img
```

Verified the assembled boot header, appended DTBs, empty `second`, embedded
ADT, RAM base/size, console/physical geometry, logical canvas, artwork
properties and exact GPIO-only cmdline. The ADT matches 0185 except for its
fresh random seed; product traits match the confirmed homescreen baseline.
Fastboot reported successful transfer and temporary boot.

**Awaiting the on-screen result.** Photograph the `DAISY TOUCH: L10`, `L6`,
`GPIO SDA...`, and `GPIO id...` lines after 30–60 seconds. Successful identity
should contain `GPIO id r0 reg9f stage7 id5422`; a following firmware/vendor
line reports each read's own status. A GPIO success proves identity transport
only, not a working report decoder or iOS touch. The 0185 comparison result
has not been supplied and must not be inferred from this boot.

## 0186 observed rails / opt-in RPM power (0187)

User-transcribed 0186 results:

```text
DAISY TOUCH: GPIO id ... stage2 ... tries3
DAISY TOUCH: L10 p149 r0 type 21/01 st08 en00 v00/54
DAISY TOUCH: L6 p145 r0 type 21/00 st02 en00 v00/00
```

The exact GPIO ID line was not legible enough to treat as a complete record;
it appears consistent with `r-2 rega3 stage2 id0000 tries3` (write-address NACK).
L10's `en00` was transcribed as `an00`; the original photograph has not been
supplied. Both rails report successful observer reads and clear software-enable
and ready bits, strongly supporting missing supplies after fastboot. The full
GPIO SDA/SCL/reset/IRQ-level line is still needed. The older QUP failure lines
were also transcribed, but do not establish an 0185 STOP/RESTART outcome.

`0187-daisy-touch-rpm-power.patch` adds a separate, opt-in legacy SMD RPM client
in `bsd/dev/daisy_touch_rpm.c`. It is only called from the GPIO probe when all
three selectors are set:

```text
daisytouch=1 daisytouch_gpio=1 daisytouch_power=1
```

All remain off by default. It requests only the active-set `ldoa` L10 2.85 V
and L6 1.8 V supplies named by Daisy's recovered FT5446 DT. Each request contains
`swen=1` and `uv=<target>`; L10 precedes L6 as in the vendor driver's VDD/IOVCC
enable order. No L5, sleep votes, direct PMIC writes, or touchscreen firmware /
configuration writes are made. It still emits no HID events.

Unlike the Mi A1 reference, Daisy's recovered `qcom,smd-rpm` node uses the SMEM
parent IRQ register `0x0b011008`, offset 0, bitmask 1, edge 15. The Mi A1 code's
`0x0b111008` is **not** used. This differs from Pronto's bit 17 at the same Daisy
register. Linux v6.12 `qcom_smd.c`, `smd-rpm.c` and `qcom_smd-regulator.c` were
cross-checked; local references are in
`artifacts/daisy-touch-identification/rpm-reference/`.

The client validates legacy SMEM allocation bounds and alignment, exact
`rpm_requests` name / edge / packet type / reference count, CID bounds, the
88-byte word-flag channel info and power-of-two FIFO size before modifying
channel state. The CLOSED/OPENING/OPENED handshake and later MMIO writes touch
only locally owned fields. TX capacity includes the 20-byte SMD header and a
one-word gap; ring indices and lengths are checked before access, including
wraparound. Replies must have a bounded complete SMD packet, matching service
and payload lengths, well-formed records, and one matching request-ID record.
RPM error records fail the request. Stale IDs are consumed but never accepted
as the current ACK; processing is bounded. Open/TX-space/ACK waits are bounded
at roughly 1/1/2 seconds respectively, with 5 ms kernel-thread sleeps rather
than a multi-second busy spin. Votes are attempted once per boot, not retried.

Before voting, the GPIO path requires readable ULT-LDO type `0x21` for both
observer channels. It holds reset low during RPM initialization and requests.
Afterwards it reads enable/ready bit 7 on each rail and prints fresh raw rail
snapshots. Only successful replies **and** ready/enabled readback permit the
vendor reset/settling sequence and GPIO identity retry. Failures leave reset
low and SDA/SCL released, with optional repeated diagnostics but no repeated
votes. A successful L10 vote followed by a failed L6 vote may leave L10 enabled;
there is deliberately no speculative rollback. RPM/readback success is still
not a measured voltage or proof of working touch.

New summary lines include `DAISY RPM: cid...`, per-vote `L10/L6 uv... id... r...`,
`power result ...`, and `DAISY TOUCH: RPM r... rails-ready...`. RPM errors:
`-1` map/layout, `-2` channel-open timeout, `-3` invalid indices/disconnected,
`-4` no TX space, `-5` ACK timeout, `-6` malformed reply, `-7` RPM error,
`-8` too many stale IDs, `-9` non-whitelisted vote. The GPIO caller's `-10`
means missing/unreadable/unexpected rail type, with no RPM request made.

Host tests cover the actual RPM C with a deterministic legacy-SMEM/SMD peer:
allocation/name/type/CID/layout/map rejection, exclusive field writes,
handshake failure, exact voltage/enable requests, full/wrapped FIFOs,
invalid indices, delayed/missing/malformed/error/stale-ID replies, and no
duplicate calls/retries. Existing input tests additionally verify the default
and selector behavior, pre-vote type checks, reset-held failures and successful
GPIO identity after ready readback. GCC sanitizers pass both suites; device
behavior and XNU SDK compatibility still require CI/hardware validation.

### RPM power diagnostic image — 2026-10-29

CI run [40922494438](https://github.com/proxer05/ios-vince/actions/runs/40922494438)
passed for commit `9429329f6b152bca0cd88d40626f005e916376d8`, retaining
`MACHINE_CONFIG=VINCE`. The build log confirms 0187 applied and compiled,
and the linked Bluetooth, QUP, GPIO and RPM marker checks passed. The downloaded
kernel matches its CI `SHA256SUMS`.

- Kernel: `artifacts/kernel-0187/kernel.development.vince`.
- Kernel SHA-256: `5112738587a013d5ea81c61e09bbca6585bed0192c9c772e802646970119a339`.
- Image: `artifacts/daisy-touch-power-0187/daisy-boot.img` (17,917,952 bytes).
- Image SHA-256: `6786a9a847ccd36ca7c4e02591082e5d7defe34c2c19475be70e41bdcd790647`.
- Logs: `artifacts/daisy-touch-dev/ci-build-0187.log` and `build-image-0187.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0187/kernel.development.vince artifacts/daisy-touch-power-0187
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-power-0187/daisy-boot.img
```

Verified the assembled boot header, appended DTBs, empty `second`, embedded
ADT, RAM base/size, console/physical geometry, logical canvas, artwork
properties, exact power/GPIO cmdline and linked markers. The ADT matches 0186
except for the fresh random seed; product traits match the confirmed homescreen
baseline. No `sweethidtest` is supplied. Fastboot reported successful transfer
and temporary boot; the boot partition was not flashed.

**Awaiting on-screen results.** After 30–60 seconds, photograph the entire
`DAISY RPM:` / `DAISY TOUCH:` block, particularly the per-rail vote results,
fresh L10/L6 snapshots, `RPM r... rails-ready...`, GPIO levels and identity.
The success targets are `RPM r0 rails-ready1` followed by
`GPIO id r0 reg9f stage7 id5422`. A failed request or ready readback holds reset
low; those diagnostics must not be interpreted as an attempted GPIO ID read.
Neither successful fastboot nor CI establishes successful RPM or touchscreen
hardware operation.

## 0187 device result / read-only RPM layout diagnostic (0188)

User-transcribed 0187 screen output:

```text
DAISY TOUCH: GPIO probe 0x36: QUP bypass, no HID
DAISY TOUCH: rail observer arb 20610000 (read only)
DAISY TOUCH: L10 p149 r0 type 21/09 st00 en00 v00/54
DAISY TOUCH: L6 p145 r0 type 21/00 st82 en00 v00/00
DAISY TOUCH: RPM: power result -1
DAISY TOUCH: L10 p149 r0 type 21/09 st00 en00 v00/54
DAISY TOUCH: L6 p145 r0 type 21/2a st82 en00 v00/04
DAISY TOUCH: RPM r-1 rails-ready0
```

The linked binary's literal is `GPIO probe 0x38` and RPM messages begin
`DAISY RPM:`, so `0x36` and `DAISY TOUCH: RPM: power result` are treated as
photograph/transcription ambiguity, not changes to the configured address or
code. `power result -1` is unambiguous: 0187 rejected the SMEM/SMD channel
layout before opening the channel or sending L10/L6 votes. Reset remained low,
and no GPIO identity transaction followed. Therefore this boot made **no RPM
power request** and does not establish whether either vote would work.

The observer reports are not yet reliable enough to interpret electrically.
L10's subtype remained `09`, but L6 changed from `00` to `2a` even though a
PMIC peripheral subtype cannot change at runtime. L6 `st82` has bit 7 set while
`en00` is clear; L10 has neither. These are raw register values, not voltages,
and the inconsistent subtype strongly suggests stale/unstable observer data
or a transcription issue. The earlier assumptions about both ready bits being
clear must not be carried forward.

Patch `0188-daisy-rpm-layout-diagnostic.patch` adds a separate read-only mode:

```text
vincetouch=0 daisytouch=1 daisytouch_gpio=1 daisytouch_rpm_diag=1 daisytouch_probe=1
```

`daisytouch_rpm_diag` defaults off, requires Daisy GPIO mode, and supersedes
both `daisytouch_power` and identity probing if they are also present. It maps
the same regions and reads only SMEM allocation/channel records. It performs
no SMD state/FIFO writes, no doorbell, no RPM votes, no PMIC writes, no I²C,
and no HID. GPIO64 remains asserted low; GPIO10/11 remain released. Optional
diagnostics repeat at 30 and 60 seconds.

The output identifies the exact rejected stage without relaxing a check:

- stage 1: primary SMEM or Daisy IPC mapping;
- stage 2: primary SMEM item 13 allocation/bounds/mapping;
- stage 3: no exact `rpm_requests` packet channel for edge 15 in item 13;
- stage 4: channel-info item `14 + cid` missing or not exactly 88 bytes;
- stage 5: FIFO item `338 + cid` missing, too small, or not power-of-two;
- stage 6: layout accepted (still no writes in diagnostic mode).

It prints SMEM heap metadata, item allocation/offset/size/aux-base, bounded
printable candidate channel records, and selected CID. Names are copied into a
terminated local buffer; at most eight candidates print per pass. External,
unaligned, out-of-range and unsupported allocations remain rejected. A later
patch may support a discovered layout only after its physical region and
bounds are independently validated.

Linux `qcom_smd.c` scans both legacy allocation tables: item 13 with info/FIFO
bases 14/338 and item 266 with bases 138/202. 0188 therefore also reports item
266, its candidates, and the selected channel's item metadata. This secondary
scan is diagnostic-only: `dr_layout()` and the power path still accept only the
previously implemented item-13 layout. A valid item-266 channel cannot enable
channel writes, a doorbell, or rail votes.

The same Linux driver accepts either a 40-byte byte-aligned channel-info pair
or an 88-byte word-aligned pair. The 0187 power implementation deliberately
still accepts only the 88-byte format it implements; 0188 reports an observed
40-byte item as stage 4 rather than silently interpreting it. Linux also halves
the FIFO item into equal TX/RX rings and uses power-of-two masking, matching the
existing bounded FIFO check. Linux SMEM resolves nonzero `aux_base` only against
a configured memory region; 0188 prints that raw field but does not broaden the
physical mapping whitelist.

0188 also hardens the read-only observer diagnostic. Error flags are accepted
only with DONE, matching Linux's PMIC-arbiter ordering. Each requested byte is
issued and read independently twice; only agreeing values are published, with
up to three bounded pairs. `r-101` means unstable readback, not a rail state.
A short post-command delay reduces reuse of prior DONE/data. This does not make
raw selectors a voltage measurement.

Host tests exercise the actual patched sources with GCC and Clang sanitizers.
They verify stage 1–6 classification, both Linux allocation tables, secondary
table diagnostic-only behavior, and zero channel/FIFO/doorbell writes in layout
mode; selector precedence over power/identity; reset-low and released bus lines;
agreeing reads; stale error-without-DONE timeout; completed denied reads;
unstable read rejection; and all prior transport/RPM safety cases.

The Daisy vendor FT5446 driver was rechecked before publishing 0188. It still
confirms `vdd` from L10 (DT-fixed 2.85 V), `vcc_i2c` from L6 (1.8 V), reset low
for 20 ms then a 200 ms settle, IDs at `0xa3`/`0x9f`, and a combined register-0
read of `3 + 10 * 6 = 63` bytes. No L5 vote or HID-before-identity behavior is
supported by that Linux evidence.

### RPM layout-only diagnostic image — 2026-10-29

CI run [40931904580](https://github.com/proxer05/ios-vince/actions/runs/40931904580)
passed for commit `b9a4c9b6c2864e73c41a760999abbf69054c7c7e`, retaining
`MACHINE_CONFIG=VINCE`. The log confirms 0188 applied, `daisy_touch_rpm.c`
compiled, and all linked Bluetooth/touch/RPM diagnostic markers passed. The
downloaded kernel matches CI's `SHA256SUMS`.

- Kernel: `artifacts/kernel-0188/kernel.development.vince`.
- Kernel SHA-256: `725e05cf74bea2ec6a2dd07e90304bd6e509af0dffa5bdfb93b6d1baeaa1beac`.
- Image: `artifacts/daisy-touch-layout-0188/daisy-boot.img` (17,920,000 bytes).
- Image SHA-256: `8ae287175829a748232c6c5be5c3a22d7d376ce49b08b1473643badffdffd200`.
- Logs: `artifacts/daisy-touch-dev/ci-build-0188.log` and
  `build-image-0188.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_rpm_diag=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0188/kernel.development.vince artifacts/daisy-touch-layout-0188
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-layout-0188/daisy-boot.img
```

Verified the Android v0 header, exact appended Daisy DTBs, empty `second`,
embedded ADT and payload, RAM/geometry properties, exact layout-only cmdline,
and linked 0188 markers. Neither `daisytouch_power` nor `sweethidtest` is in the
cmdline. Fastboot reported successful transfer and temporary boot in 0.539 s;
no partition was flashed.

**Awaiting on-screen results.** After at least 60 seconds, capture the complete
`DAISY RPM:` and `DAISY TOUCH:` block. The required evidence is the heap line,
item 13 and item 266 metadata/candidates, selected CID, stage/result, all L10/L6
observer lines, reset/SDA/SCL levels, and the repeated 30/60-second output. This
image performs no RPM/SMD/PMIC/I²C writes and emits no HID events.

## 0188 device result / Daisy Linux RPM audit (0189)

User-transcribed 0188 output:

```text
DAISY RPM: item13 a1 off6710 size800 ext0
DAISY RPM: table13 ch4 rpm_requests cid4 flags20f ref1
DAISY RPM: table13 ch5 rpm_requests cid5 flags210 ref1
DAISY RPM: table13 ch6 rpm_requests cid6 flags212 ref1
DAISY RPM: table13 ch7 rpm_requests cid7 flags213 ref1
DAISY RPM: table13 ch8 rpm_requests cid8 flags211 ref1
DAISY RPM: table13 used5 candidates5 selected4
DAISY RPM: item18 a1 off0 size58 ext60000
DAISY RPM: item266 a0 off0 size0 ext0
DAISY RPM: layout result -1 stage4 (no writes)
DAISY TOUCH: L10 p149 r0 type 21/09 st00 en00 v00/54
DAISY TOUCH: L6 p145 r0 type 21/2a st82 en00 v00/04
DAISY TOUCH: layout-only reset0 SDA1 SCL1 irq1
```

The selected channel is CID4 on edge 15 (`0x20f`). Item18 (`14 + 4`) has
the expected `0x58`/88-byte size, but resides at auxiliary base `0x60000`,
offset zero. The primary-only mapper rejects this at stage 4; item342
(`338 + 4`, the FIFO) was therefore not inspected. Item266 is unallocated.
The heap line and complete repeated snapshots were not supplied. The rail
lines still do not establish reliable enable/ready readback or measured voltage.

At the user's request, audited the **Daisy vendor Linux tree**:
[MiCode/Xiaomi_Kernel_OpenSource, daisy-o-oss, commit
6421aa5cdc56e9ef7c6329e0a05164503e1513ef](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/6421aa5cdc56e9ef7c6329e0a05164503e1513ef).
The relevant source files are preserved with their original paths under
`artifacts/daisy-touch-identification/linux-daisy-6421aa5/`; all 16 downloaded
files were verified against that commit's Git tree blob IDs.

Key source locations (relative to that Linux tree):

- `drivers/soc/qcom/smem.c:301–333`: the legacy TOC's `reserved &
  BASE_ADDR_MASK` is the physical region base; zero selects primary SMEM.
  `smem_phys_to_virt()` at line 199 resolves against configured regions;
  auxiliary regions are mapped with `ioremap_nocache()` during probe near
  line 1385. An auxiliary item may legitimately start at offset zero.
- `arch/arm/boot/dts/qcom/msm8953-daisy.dtsi:1136–1155,1210–1218`:
  `rpm_requests`, edge 15; primary SMEM `0x86300000/0x100000`, auxiliary
  SMEM `0x60000/0x8000`, and RPM doorbell `0x0b011008`, offset 0, mask 1.
- `drivers/soc/qcom/smd_private.c:267` explicitly uses word access for RPM
  edges. `smd_private.h:80–92` defines the 44-byte half-channel; `smd.c:1672`
  resolves info/FIFO items independently and splits the FIFO equally into
  send/receive rings. Table bases come from `include/soc/qcom/smem.h`.
  `smd.c:1043,1551` reserves four bytes and prefixes packets with five words
  (payload length followed by four zeros).
- `drivers/soc/qcom/rpm-smd.c:112–128,1199–1286,1878` opens the named SMD
  edge and serializes `req\0`, request length, message ID, set, resource type,
  resource ID, data length, then key/length/value records. Active set is zero
  (`include/soc/qcom/rpm-smd.h:22`). Its ACK code at lines 359–409 extracts the
  request ID and checks trailing `err\0` data. These match our packet layout;
  XNU additionally bounds complete replies and rejects malformed records.
- `drivers/regulator/rpm-smd-regulator.c:130–131,434–447` uses `swen` and `uv`
  with four-byte values. `msm-pm8953-rpm-regulator.dtsi:159–172,223–236` maps
  L6/L10 to `ldoa` IDs 6/10. `msm8953-regulator.dtsi:170,219` fixes their
  voltages at 1.8 V/2.85 V. The recovered touchscreen DT and vendor FT5446
  driver establish their consumer and enable order as documented above.

`0189-daisy-rpm-aux-smem.patch` admits exactly the DT-proven auxiliary region
alongside primary SMEM. It validates the complete allocation and rounded map
span before mapping, permits offset zero only for the auxiliary region, and
prints each accepted item's physical address. The existing opt-in layout
diagnostic additionally snapshots TX/RX states and head/tail indices with a
`bounds` flag. The snapshot does not consume FIFO contents; `bounds1` describes
index validity only, not a completed handshake. Repeated layout checks clear
previously selected channel pointers before resolving the current TOC.

GCC and Clang ASan/UBSan suites pass. New actual-source RPM cases reproduce
the observed five-channel table and CID4 selection, exercise independent
primary/auxiliary info and FIFO placement, offset zero, page crossing, end-of-
region bounds, reserved base bits, map failures, and reset of stale selection.
Synthetic auxiliary FIFO placement is a test fixture, not a device observation.
Memory snapshots and write counters verify diagnostic-only operation; the mock
peer verifies exact votes/ACKs through each mapping combination. Existing input
tests also pass for selector precedence, reset handling and stable rail reads.

The next temporary boot uses `daisytouch_rpm_diag=1` to capture item342,
the accepted physical ranges, states/indices, and repeated rail observations.
Power remains separately opt-in; successful RPM/rail readback, identity and
coherent touch reports still require hardware evidence.

### Auxiliary-SMEM layout diagnostic image — 2026-10-29

CI run [40938184413](https://github.com/proxer05/ios-vince/actions/runs/40938184413)
passed for commit `b3be16b8e469e3ca9cbcec912e8b75e85fbe0ed0`, retaining
`MACHINE_CONFIG=VINCE`. The log confirms 0189 applied, `daisy_touch_rpm.c`
compiled, and the linked auxiliary-mapping/snapshot markers passed. The
downloaded kernel matches CI's `SHA256SUMS`.

- Kernel: `artifacts/kernel-0189/kernel.development.vince`.
- Kernel SHA-256: `19a7d68bc0d4b9ce92d55e078f79b66717c0835a2d4a66ee4eb8e6e6561e8d63`.
- Image: `artifacts/daisy-touch-layout-0189/daisy-boot.img` (17,920,000 bytes).
- Image SHA-256: `dbec6edd14d3bea8b3b8667b539a3c2c012f4e337c2149c96213812310d647b1`.
- Logs: `artifacts/daisy-touch-dev/ci-build-0189.log` and
  `build-image-0189.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_rpm_diag=1 startup_debug=0 -disable_kprintf' \
tools/build_daisy.sh artifacts/kernel-0189/kernel.development.vince artifacts/daisy-touch-layout-0189
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-layout-0189/daisy-boot.img
```

Verified the Android v0 header/hash, appended Daisy DTBs, empty `second`,
embedded payload/ADT, exact cmdline, RAM bounds, every kernel segment, and
0189 markers. The complete ADT matches 0188 apart from its fresh random seed,
including geometry/artwork properties. Fastboot reported successful transfer
and temporary boot on serial `6790a29e0305` in 0.537 s.

**Awaiting on-screen results.** After at least 60 seconds, capture the complete
`DAISY RPM:` / `DAISY TOUCH:` block, especially mapped item18, item342 metadata
and mapping, the `snapshot half... state... tx... rx... bounds...` line,
layout result/stage, and L10/L6 observations. Include any repeated 30/60-second
output. The cmdline selects layout-only diagnostics with reset held low;
it supplies neither `daisytouch_power` nor `sweethidtest`.

### 0189 screen transcription and exact-layout replay

The next user transcription reports heap `init1 free6f98 avail27068 ver7b0000`,
item13 `off6718 size800 ext0`, selected CID4, item18 `off0 size58 ext60000`,
and item342 `off800 size800 ext60000`. The snapshot is:

```text
DAISY RPM: snapshot half400 state0/4 tx120/120 rx70/70 bounds1
```

Reaching this snapshot means `dr_layout()` returned success (stage 6). The
final result line was omitted. The FIFO is 2048 bytes with two 1024-byte rings;
both rings are empty with aligned, in-range indices. Linux names states 0/4
CLOSED/CLOSING. This is a pre-open snapshot, not an RPM handshake failure.
Vendor `smd.c:1279–1331,1930–1958` handles close completion and initiates OPENING;
the XNU client retains its bounded OPENING/OPENED handshake from mainline SMD.

The transcribed mapped addresses (`86386718`, `68898`, `68800`) disagree with
the supplied TOC and the code's `base + offset` print. Derived addresses are
`86306718`, `60000`, `60800`; these are expected values, not corrected device
observations. The `GPIO probe 0x36` transcription also differs from the linked
`0x38` literal. Raw logs are needed to resolve these discrepancies.

L10/L6 again report `21/09 ... v00/54` and `21/2a ... v00/04`. In the pinned
vendor `drivers/regulator/qpnp-regulator.c`, **both subtypes are valid**:
ULT-LDO P150 (`09`) and LV_P300 (`2a`), mapped at lines 1685/1689 to
`ult_pldo_ranges` (line 453). Its selector formula is 1,750,000 + 12,500 × VSET
microvolts: `54`/`04` encode 2.80 V/1.80 V settings, not measured outputs.
L10's requested 2.85 V would encode `58`. Both reported `en00`; L6's earlier
subtype changes still make complete, repeated raw records important.

GCC/Clang sanitizer tests now replay the reported item13 offset, exact auxiliary
info/FIFO placement, size and state/indices. They verify the derived address
strings and zero diagnostic writes. The user requested persistent eMMC logging
to replace screenshot transcription before continuing the hardware diagnosis.

The 0190 kernel now includes the opt-in HFS console worker (`daisylog=1`) and
the requested upstream GPU refresh. CI `40944810745` passed, and the validated
layout-only logging image was temporarily booted on serial `6790a29e0305`.
See [boot diagnostics](BOOT-DEBUG.md#0190-logging-image-ci-passed-and-temporary-boot-sent)
for hashes and reproduction. Next collect `/private/var/log/daisy-console.log`
through TWRP to resolve the address/state transcription before a power test.

### 0190 raw eMMC log: mapping ambiguity resolved

Recovered `artifacts/console-0190.log` through TWRP (24,817 bytes;
SHA-256 `69aa08b03c877e4d43add25118e86507e61c529b9507c025d555ef201d8bfe62`).
The raw file establishes the previously ambiguous literals and addresses:

```text
DAISY TOUCH: GPIO probe 0x38; QUP bypass, no HID
DAISY TOUCH: rail observer arb 20010000 (read only)
DAISY RPM: mapped item13 pa86306718 size800
DAISY RPM: mapped item18 pa60000 size58
DAISY RPM: mapped item342 pa60800 size800
DAISY RPM: snapshot half400 state0/4 tx120/120 rx78/78 bounds1
DAISY RPM: layout result 0 stage6 (no writes)
DAISY TOUCH: L10 p149 r0 type 21/09 st00 en00 v00/54
DAISY TOUCH: L6 p145 r0 type 21/2a st82 en80 v00/04
DAISY TOUCH: layout-only reset0 SDA1 SCL1 irq1
```

Heap and allocation metadata match the 0189 replay fixture; CID4 is selected,
item266 is unallocated, and both FIFO halves are empty with valid indices.
The actual physical addresses agree with the bounded mapper arithmetic. No
mapping changes are justified. The actual observer version is `0x20010000`,
not the earlier transcribed `0x20610000`.

Both pre-layout and post-layout rail reads agree within this capture. L6 now
reports `en80` with status `82`: enabled/ready bits are set, and the voltage
selector encodes 1.80 V. L10 remains disabled/not-ready with a 2.80 V selector.
These are register observations, not measured rail voltages. The diagnostic
made no RPM votes and held reset low; FT5446 identity remains untested here.

The user confirmed deliberately rebooting early; normal boots reach the
lockscreen. This explains why the saved file contains only the first layout
pass. Early-log persistence is validated. Allow at least 90 seconds on the next
diagnostic boot before rebooting to recovery for the 30/60-second comparisons.

### 0190 complete lockscreen log

After the user reported reaching the lockscreen and returning to fastboot,
saved the newer on-device console file before starting the next test:
`artifacts/console-0190-lockscreen.log`, 174,453 bytes, SHA-256
`6b38ca78c5062cf934123c30fbc147fb415d98f8f3191d61479608bb193ca92b`.

All three scheduled layout snapshots are present and identical:
`half400 state0/4 tx120/120 rx78/78 bounds1`, `layout result 0 stage6`.
The addresses remain `86306718`, `60000`, `60800`. Every L10 read is
`21/09 st00 en00 v00/54`; every L6 read is `21/2a st82 en80 v00/04`.
Reset stays asserted, with released SDA/SCL and high IRQ. Later process
sampler and SpringBoard/backboardd output confirms the logger continues
past the 30/60-second repeats. The earlier short capture no longer leaves
a sustained-logging validation gap.

### 0190 power/ACK test with persistent logging

Reused the checksum-verified kernel from CI `40944810745` at `0278296`.
The new image replaces `daisytouch_rpm_diag=1` with `daisytouch_power=1`,
retaining `daisylog=1`, GPIO transport, and repeated diagnostics. Omitting
the layout-only selector is essential because it supersedes the power path.

The existing bounded implementation opens the verified CID4 channel, requests
L10 at 2.85 V and L6 at 1.8 V, checks matching ACK request IDs/errors, then
requires both rails enabled/ready before releasing reset and attempting GPIO
FT5446 identity. Any power/rail failure keeps reset asserted. Touch HID events
remain disabled; this test does not yet establish 63-byte report transport.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf daisylog=1' \
tools/build_daisy.sh artifacts/kernel-0190/kernel.development.vince artifacts/daisy-touch-power-0190
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-power-0190/daisy-boot.img
```

- Image: `artifacts/daisy-touch-power-0190/daisy-boot.img`, 17,928,192 bytes.
- Image SHA-256: `08ed4938acc7b514cce2110c3562f6be097e01fce8836dada5b30210a69e2711`.
- Kernel SHA-256: `cf464faa3b28090c6c04591c1f74a914fdb086482f3bd9194d5b2f67676b2d94`.
- Build log: `artifacts/daisy-touch-dev/build-image-power-0190.log`.

Validated the boot header/hash, appended DTBs, empty `second`, complete
payload/ADT, every kernel segment and RAM bounds, exact power-test cmdline,
and linked power/rail/GPIO/logger markers. Fastboot reported successful
transfer and temporary boot on serial `6790a29e0305` in 0.543 s.

**Hardware result pending.** Allow 90 seconds, return to fastboot, temporarily
boot TWRP, and save the next console to `artifacts/console-power-0190.log`.
Required evidence: channel-open result, L10/L6 vote/ACK lines, rail readback,
`rails-ready`, and any GPIO identity/transfer results and repeated samples.

### 0190 power test result: RPM and GPIO identity validated

After the test, temporarily booted TWRP on serial `6790a29e0305` (fastboot
success in 1.794 s) and extracted the complete saved console:

```sh
python3 tools/pull_daisy_console.py artifacts/console-power-0190.log --serial 6790a29e0305
```

- Size: **283,516 bytes**, 3,084 lines.
- SHA-256: `0d9ca1525bb5a4048e226c504209555d3c92f31f5e98059b53cf3446b627769f`.

The existing handshake successfully reopens the previously observed
CLOSED/CLOSING channel. The bounded mapper still uses the same proven
primary/auxiliary locations. The two active-set requests each complete once:

```text
DAISY RPM: half1024 states0/4 ipcb011008
DAISY RPM: L10 uv2850000 id1 r0 ack 716572/c/2367736d
DAISY RPM: L6 uv1800000 id2 r0 ack 716572/c/2367736d
DAISY RPM: power result 0
DAISY TOUCH: L10 p149 r0 type 21/09 st80 en80 v00/58
DAISY TOUCH: L6 p145 r0 type 21/2a st82 en80 v00/04
DAISY TOUCH: RPM r0 rails-ready1
DAISY TOUCH: GPIO SDA1 SCL1 reset1 irq1 (external pulls)
DAISY TOUCH: GPIO id r0 reg9f stage7 id5422 tries1
DAISY TOUCH: GPIO FT5446 fw r0:0c vendor r0:51; probe only
```

`r0` for each vote means the ACK parser accepted the expected request ID with
no RPM error. L10 changes from `st00 en00 v00/54` to `st80 en80 v00/58`;
L6 stays `st82 en80 v00/04`. Those selectors encode the requested 2.85 V and
1.80 V settings; they are not external voltage measurements. Both rails pass
the independent enabled/ready gate before reset is released.

All **three identity probes** (initial, 30 seconds, 60 seconds) return
`id5422 tries1`, firmware `0c`, vendor `51`. Both repeated rail summaries also
match the successful post-vote values. This validates RPM power and the GPIO
identity/register-read path; supplying the specified rails resolves the GPIO
probe's earlier NACKs. It does not establish QUP operation or report coherence.

The log also includes process-sampler passes 0 and 1 and later userspace
activity, with no logger-stop/loss markers. The phone is left in TWRP after
collection. Next add an opt-in coherent register-0 **63-byte report** capture
using the validated power/identity sequence, retaining ten-slot/1080×2280
geometry and the existing HID transform. Confirm complete idle and moving
contact frames before enabling contact decoding or touch event delivery.

### 0191 opt-in GPIO report capture

`0191-daisy-gpio-report-capture.patch` adds `daisytouch_reports=1`. It requires
`daisytouch=1 daisytouch_gpio=1 daisytouch_power=1`, successful RPM/rail gating,
and FT5446 identity plus firmware/vendor reads. The layout-only selector still
supersedes power and report capture. Every selector remains disabled by default.

Each frame is one combined register-0, repeated-START **63-byte** read, matching
the pinned vendor driver's `fts_read_touchdata()` transaction: ACK the first
62 bytes, NACK the last, then STOP. A temporary buffer is published only after
the entire read and STOP succeed. The one-byte identity helper uses this same
transport. Reset timing, external I²C pulls, ten-slot/1080×2280 format, and
the existing HID transform are preserved; the capture sends no touch events.

After successful identity, the worker polls with a 20 ms sleep for at most
five minutes. It records complete changed frames and one idle sample per
second, up to 4,096 raw frames; eight consecutive I²C failures stop it early.
Each raw line includes sequence, elapsed milliseconds, IRQ before/after,
received length, and all 126 hex digits. The final summary reports successful
reads, errors, raw lines, and stop reason. `daisytouch_probe=1` retains rail
and identity checks near 30/60 seconds; identity loss ends capture.

Actual-source host tests (`python3 tools/tests/test_daisy_touch_probe.py`, also
with `CC=clang`) pass ASan/UBSan: lengths 1–63, exact START/address/register and
ACK/NACK/STOP behavior, timeouts at every SCL-high phase, bounded stretching,
address/register NACKs, atomic output, invalid lengths, GPIO/power/identity
gates, time/frame/error limits, repeated checks, and absence of touch HID.
These establish software behavior; hardware report coherence remains pending.

Hardware sequence: boot the 0191 image with the 0190 power-test arguments plus
`daisytouch_reports=1`, retain `daisylog=1`, leave the screen untouched briefly,
then perform separated taps, slow swipes across the screen, and two-finger
movement with full releases. Return to fastboot after five minutes so the
capture's final counters are persisted. Temporarily boot TWRP and extract to
`artifacts/console-reports-0191.log`.
Host inspection must check complete frames, normal mode, valid counts/events/
unique slot IDs/coordinates, agreement between active records and header count,
and observed idle → contact → motion → release transitions before HID work.

`python3 tools/analyze_daisy_reports.py artifacts/console-reports-0191.log`
checks those fields against the pinned vendor layout, requires the logged
power/identity gates, monotonic sample metadata, and consistent completion
counters. It summarizes observed slots, coordinate ranges, events, motion,
multitouch and full releases. Exit 0 requires complete, error-free evidence
covering all those states; exit 1 denotes malformed/failed capture evidence,
and exit 2 denotes missing coverage or completion. Unchanged frames are
suppressed in the kernel, so gaps in sequence numbers are expected. UP records
can coexist with active records; the header count excludes UP. Five host tests
cover recorded-format fixtures, corruptions, transport failures and incomplete
captures (`python3 tools/tests/test_daisy_report_analysis.py`).

0191 kernel CI: `40952756300` at `38f315159658247a481e90f1973c75e0581f0579`,
with `MACHINE_CONFIG=VINCE` and linked report-capture marker checks.

### 0191 report-test image built and temporarily booted

CI `40952756300` passed compilation and all linked marker checks. Downloaded
`xnu-kernel-vince` to `artifacts/kernel-0191/` and verified its `SHA256SUMS`.
Full CI log: `artifacts/daisy-touch-dev/ci-0191-success.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf daisylog=1 daisytouch_reports=1' \
tools/build_daisy.sh artifacts/kernel-0191/kernel.development.vince artifacts/daisy-touch-reports-0191
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-reports-0191/daisy-boot.img
```

- Kernel SHA-256: `bd0f30acc4ea14a070b53c4f8ca29bd73a35be4ca2bc92def6b99f6b64fcd3b8`.
- Image SHA-256: `11cb751fe6f9daa33efd51d7bbc8b7848495f16fb8036f7a493c4310a665489b`.
- Image size: 17,928,192 bytes.
- Build log: `artifacts/daisy-touch-dev/build-image-reports-0191.log`.

Verified the Android header/hash, appended Daisy DTBs, empty `second`, complete
payload/ADT, RAM bounds and all kernel segments, exact command line, and linked
report/power/identity/logger markers. Confirmed `product: daisy` and temporarily
booted serial `6790a29e0305`; fastboot succeeded in 0.538 s.

**Hardware report result pending.** The user was asked for approximately ten
seconds of idle, separated taps, slow swipes and two-finger movement with full
releases, then five minutes total runtime before returning to fastboot. Retrieve
`artifacts/console-reports-0191.log` through temporary TWRP and run the analyzer.
The capture is diagnostic-only and emits no touch HID events.

### 0191 result: live contacts captured, coherence unresolved

Temporarily booted TWRP on serial `6790a29e0305` (fastboot success in 1.777 s)
and saved the entire catalog-visible console file:

```sh
python3 tools/pull_daisy_console.py artifacts/console-reports-0191.log --serial 6790a29e0305
python3 tools/analyze_daisy_reports.py artifacts/console-reports-0191.log
```

- Saved file: **91,441 bytes**, 965 lines.
- SHA-256: `61c5d3a5baf140b3e5452effa3180389b6d086739d22facfd573f33040ef1d60`.
- Raw samples: **293 complete 63-byte lines**, through seq1955 at 111,067 ms.
- Both RPM votes again return `r0`; `rails-ready1` precedes capture.
- Initial and 30/60-second checks all return `id5422 tries1`, firmware `0c`,
  vendor `51`; L10/L6 enabled/ready readbacks agree throughout those checks.
- No report-transfer error, logger-stop/loss, or kernel panic markers appear.

The user confirmed returning to fastboot after **about two minutes**. That
explains the saved interval and missing five-minute completion summary; it
does not establish a logger failure. The final logged line is complete.

The analyzer accepts **204 samples structurally**: 180 with active contacts,
24 with zero active contacts, four with two active contacts, 152 motion
transitions without crossing a malformed sample, and 14 full-release transitions. Slots
0 and 1 are observed, with raw active-coordinate ranges X 0–1074 and
Y 493–1985. These are observed ranges, not a replacement for the 1080×2280
geometry or proof that all slots/edges have been exercised.

The **89 rejected samples** have exactly two forms:

1. **84 startup samples**, seq1 through seq1488 (0–85,385 ms), contain byte 0
   `00` followed by 62 `ff` bytes, with IRQ `1/1`. Their count nibble is 15,
   so they are not valid zero-contact frames. This pattern stops before the
   first accepted contact at seq1498, 85,951 ms, and never recurs in the saved
   contact interval. It is consistent with data not yet being ready during
   unconditional startup polling, but that interpretation is not yet proven.
2. **Five contact samples** have header count 2 and event **3** in the second
   record (slot 1). Every first record has event 2. The second records are:

   | Sequence | Elapsed ms | Raw six-byte record | Raw X / Y |
   | --- | --- | --- | --- |
   | 1662 | 94967 | `c00016173030` | 0 / 1559 |
   | 1664 | 95071 | `c00416183e30` | 4 / 1560 |
   | 1706 | 97512 | `c00015981520` | 0 / 1432 |
   | 1757 | 100274 | `c00016950b10` | 0 / 1685 |
   | 1863 | 105873 | `c00015173e30` | 0 / 1303 |

The pinned vendor header defines DOWN=0, UP=1, CONTACT=2. Its report path
treats non-DOWN/non-CONTACT records as released; it supplies no basis for
accepting event 3 as a second active contact. Doing so would conceal the
count/event inconsistency. Keep these frames rejected rather than masking
the event or adjusting the count.

Example accepted samples: seq1498 has slot1 DOWN at (504,1804) alongside
slot0 UP at (500,1804); seq1500 has two CONTACT records at (506,1805) and
(894,1809); seq1504 has count 0 and slot1 UP at (524,1811). These demonstrate
live report content, including mixed UP/active records, but the five suspect
samples prevent declaring the stream coherent.

Consecutive logged-read start intervals have minimum/median 50/52 ms despite
the 20 ms sleep. These include transfer, logging, and scheduling time; they
are not measured wire durations. Next instrument transaction duration and
IRQ-ready behavior to investigate the second-record anomaly and startup
pattern. The current evidence does not distinguish controller semantics,
live-buffer updates, or transport timing, and does not justify changing power,
mapping, reset timing, geometry, or enabling HID.

The host analyzer now groups repeated errors so startup failures do not hide
the five contact faults. A new regression uses exact raw device records and
retains rejection of the startup sentinel and event 3. All six analyzer tests
pass; analysis of this device capture correctly exits 1. The device is left
in TWRP after collection.

### 0192 per-byte timing and IRQ observations

`0192-daisy-report-timing-irq.patch` adds `daisytouch_trace=1` to the existing
opt-in report capture. It takes effect only inside `daisytouch_reports=1`
after the established GPIO/RPM/rail/identity gates. The trace hint by itself
does not enable capture, power votes, or touch. All options remain default-off.

The same combined 63-byte transaction now optionally measures:

- `us`: total transfer time through STOP and released-bus cleanup, before
  publishing the frame, formatting/printing it, or sleeping.
- `phase`: elapsed microseconds after the read-address ACK, header (byte 3),
  first record (byte 9), second record (byte 15), and all data (byte 63).
- `gap`: largest interval between completed data bytes, including the first
  byte measured from the read-address ACK.
- `low`: 64-bit IRQ-low sample mask; bit 0 is before START and bit N follows
  completed byte N (1–63). The existing `irqB/A` remains before/after transfer.

The measurements include scheduling and observation overhead. IRQ samples can
miss pulses; mask changes are not a hardware interrupt/edge count. Bus delays,
unconditional polling, reset timing and power sequence remain as in 0191 to
make this a timing/IRQ observation test rather than an unmeasured timing fix.
The first read's startup pattern and all malformed frame bytes remain visible.

Trace-enabled captures also print progress every roughly ten seconds and at
completion: cumulative reads/successes/errors/raw lines, min/mean/max transfer
microseconds and maximum byte gap across **all attempts**, including unchanged
frames omitted from raw logging. The five-minute/4,096-line/eight-consecutive-
error bounds remain in effect. No touch HID events are generated.

Actual-source host tests exercise IRQ bits 0–63, ordered phase timestamps,
a simulated 12,345 us scheduling pause localized to the second record, and
every SCL-high timeout with tracing both on and off. They retain complete-read/
STOP publication, error cleanup, power/identity/default-off gating, and bounded
capture checks. Hardware timing evidence is pending a new CI image and capture.

GCC and Clang ASan/UBSan suites pass. CI `40957268230` builds the kernel at
`b419648a81a236046e0ce2ad51fed4b4e46bbcb8` with `MACHINE_CONFIG=VINCE` and
linked trace/progress marker checks.

The host analyzer accepts both the original raw format and trace-v1 suffixes.
It checks phase ordering, plausible maximum byte gaps, progress counter
consistency, and that cumulative timing extrema encompass logged samples.
Timing summaries retain separate structurally-valid, startup-FF,
reserved-event and other-invalid groups, so rejecting a frame does not discard
the timing evidence needed to diagnose it. Each group reports min/median/max
transfer time and byte gap, phase medians and observed IRQ sample changes,
including the second **record's** interval (not necessarily slot ID 1).

All eight analyzer tests pass, including traced capture corruption, missing
telemetry, mismatched progress, and the actual 0191 record fixtures. Motion/
release continuity is now broken at malformed reports or transfer errors;
replaying 0191 still rejects the same 84 startup and five event-3 samples,
with the original 154 accepted-to-accepted motion transitions tightened to
152 that do not cross malformed samples. No invalid report is reclassified
as a valid contact.

### 0192 timing/IRQ image built and temporarily booted

CI `40957268230` at `b419648a81a236046e0ce2ad51fed4b4e46bbcb8` passed the
VINCE build and linked diagnostic marker checks. Downloaded the kernel to
`artifacts/kernel-0192/` and verified `SHA256SUMS`; the CI log is
`artifacts/daisy-touch-dev/ci-0192-success.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf daisylog=1 daisytouch_reports=1 daisytouch_trace=1' \
tools/build_daisy.sh artifacts/kernel-0192/kernel.development.vince artifacts/daisy-touch-trace-0192
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-trace-0192/daisy-boot.img
```

- Kernel SHA-256: `302e55322883a26969c7f0db7cea6e5fc67c12fbeadbc381ae6c0c8e935ed2fd`.
- Image SHA-256: `a688ae1684d8c1ecc8e30661b22b58858e4f9879db67d3abcfd258158f284b18`.
- Image size: 17,928,192 bytes.
- Build log: `artifacts/daisy-touch-dev/build-image-trace-0192.log`.

Verified the Android header/hash, appended Daisy DTBs, empty `second`, full
payload/ADT, RAM bounds and every kernel segment, exact command line, and
linked report/trace/progress/power/identity/logger markers. Confirmed
`product: daisy`; temporary boot on serial `6790a29e0305` succeeded in 0.538 s.

**Hardware trace result pending.** The requested interaction is initial idle,
one finger held steady for ten seconds, release, two fingers held steady for
ten seconds, then slow motion and full releases. This adds steady-contact
evidence to the previous moving-contact capture. Allow five minutes total
runtime before returning to fastboot. Temporarily boot TWRP, extract to
`artifacts/console-trace-0192.log`, and analyze with
`python3 tools/analyze_daisy_reports.py artifacts/console-trace-0192.log`.

### 0192 first trace: complete no-contact interval

The user reported that touch diagnostic lines were no longer appearing.
After returning to fastboot, temporarily booted TWRP (success in 1.775 s)
and extracted `artifacts/console-trace-0192.log`: **412,236 bytes**, 4,215 lines,
SHA-256 `58f0318dd662d81c3f683a5b1ce31b617ccff15ff787bf752a40875e82e4ebaf`.

The user then clarified they were **AFK during the five-minute window and did
not touch the screen in that interval**. The trace therefore provides a
no-contact timing baseline, not evidence of a new failure to detect touches.
It ends normally at its time limit:

```text
DAISY REPORT: progress ms300026 reads5406 ok5406 errors0 raw292 us27501/32982/145872 gap63617
DAISY REPORT: done ms300027 reads5406 ok5406 errors0 raw292 reasontime
```

All **5,406 transfers** complete without a reported I²C error. The 292 saved
raw frames are identical: `00` followed by 62 `ff` bytes, `irq1/1`, and
`low0000000000000000`. No changed frame was recorded. The count-15 sentinel
still fails report validation; it is not reinterpreted as a valid idle frame.
The three identity/firmware/vendor checks and L10/L6 ready readbacks agree.
No logger-stop/loss or kernel panic markers appear. The completed time limit
explains why diagnostic output stopped after five minutes.

Timing across **all attempts** is min/mean/max **27.501/32.982/145.872 ms**, with
a maximum completed-byte gap of **63.617 ms**. Among saved samples, transfer
min/median/max is 27.541/30.3355/110.574 ms, and median byte gap is 481 us.
Phase medians after address/header/record0/record1/all data are
2050.5/3418.5/6142/8857/30240 us. These now measure the transaction itself,
including instrumentation and scheduling, rather than the whole poll cycle.
No IRQ-low samples appear in saved frames. Contact-frame timing correlation
remains pending; these data alone do not identify the cause of 0191's event 3.

After the user explicitly confirmed readiness, reverified the **same** kernel,
image and command line, rebooted TWRP to fastboot, confirmed `product: daisy`,
and temporarily booted the image again (success in **0.533 s**). The user was
asked to begin touching during boot: one-finger and two-finger ten-second holds,
releases and slow movement within the first five minutes. The next extraction
must use `artifacts/console-trace-0192-touch.log` to preserve this baseline.
**Repeat hardware result pending.**

### 0192 contact trace: timing suspect identified

The user performed the holds/movement tests and deliberately rebooted early.
Temporarily booted TWRP (1.768 s) and saved the complete catalog-visible file
as `artifacts/console-trace-0192-touch.log`: **102,948 bytes**, 829 lines,
SHA-256 `0721efdaaf3dad659f33bced586689a4ad819ad0a3f2ed3631125a0c06976fc0`.
The last raw sample is seq699 at **41,518 ms**; the absent completion summary
is expected after that early reboot. The contact interval is useful without
another logging-only or five-minute idle test.

Of 278 raw frames, **266 pass structural checks**: 248 active, 18 zero-active,
125 two-contact, 200 motion transitions and seven full releases. Slots 0/1
cover observed raw ranges X 261–1079 and Y 1097–1489. First valid contact is
seq139 at 8,751 ms. Initial and 30-second power/rail/identity checks succeed.
The last progress record reports 674 reads, all successful, at 40,175 ms;
there are no reported I²C errors. Rejected frames are:

- Nine initial `00` + 62 `ff` sentinel frames.
- Seq143 at 9,003 ms: header 2, slot0 CONTACT (760,1457), then slot1 event 3
  at (0,1484), raw second record `c00015cc0e10`.
- Seq497 at 29,874 ms: header **1** but slot0 CONTACT (306,1205) and slot1
  DOWN (866,1107). Seq498 has header **2** with both coordinates unchanged,
  now both CONTACT.
- Seq521 at 31,374 ms: header **1** but slot0 CONTACT (310,1209) and slot1
  DOWN (885,1105). Seq522 likewise has header **2** with the same coordinates,
  now both CONTACT.

These count/record transitions support investigating report coherence around
contact appearance. They are not grounds for accepting inconsistent frames.
The analyzer regression includes exact seq497/498 bytes.

Valid logged transfers have min/median/max **28.565/31.310/56.598 ms**, with a
median largest byte gap of **478 us**. Sampled IRQ levels change during 251 of
266 valid frames and within the second record's interval in 139. The event-3
frame lasts 40.885 ms (12.491 ms before data, maximum data-byte gap 469 us).
The count-mismatch frames last 39.921/49.374 ms, with maximum data-byte gaps
469/11609 us. Large mid-data pauses are therefore not required for a malformed
frame, and IRQ changes alone do not distinguish valid from invalid content.

Source audit found a concrete candidate for the slow bit clock:
`osfmk/kern/clock.c:delay()` calls `_clock_delay_until_deadline_with_leeway()`,
which may block the thread; its spin branch calls `machine_delay_until()` in
`osfmk/arm/rtclock.c`, which executes **WFE** before rechecking the counter.
Thus `delay(5)` is not a guaranteed tight five-microsecond GPIO delay. This
path is unchanged by the port's patches. The measured ~450–480 us per data
byte warrants a GPIO-local counter-spin comparison; it does not yet prove
that delay granularity causes the malformed reports.

### 0193 opt-in GPIO counter-delay comparison

`0193-daisy-gpio-counter-delay.patch` adds `daisytouch_busywait=1`. Only GPIO
bit and stretch-poll delays use the new local absolute-counter spin. The
configured 5 us half-period and 1 us stretch step are preserved; generic
kernel delays, PMIC observer timing, reset sleeps and the 20 ms capture sleep
are untouched. Interrupts and preemption stay enabled. The option defaults
off and cannot turn touch, GPIO probing, power votes or capture on by itself.

With the option selected, a reset-held check measures 32 requested 5 us waits
using the legacy delay and 32 using the local spin, printing min/mean/max
microseconds. It issues no I²C transactions and runs after successful power
gating, before the established reset release/settle sequence. The existing
per-byte trace will measure full transfers and remaining scheduling gaps.
Layout-only diagnostics supersede this check as well as power/capture.

Actual-source tests model fivefold legacy-delay rounding and confirm the
local spin meets its requested duration without calling generic delay().
They exercise complete traced reads, ACK/NACK/STOP failure cleanup at every
clock phase, default-off/layout/power/identity gating and the delay comparison.
All report validation and HID gates remain in effect; hardware comparison is
pending the next CI kernel.

### 0193 counter-delay image built and temporarily booted

CI `40961297908` at `7aed128c699398fc958e559a2b76ac42bb20d87b` passed the
VINCE build and linked diagnostic checks. Downloaded `xnu-kernel-vince` to
`artifacts/kernel-0193/` and verified `SHA256SUMS`. Full CI log:
`artifacts/daisy-touch-dev/ci-0193-success.log`.

```sh
VINCE_NO_FBCON=0 \
DTB="$PWD/artifacts/daisy-dtb.bin" \
PAYLOAD_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1 io=0x3f -noprogress wdt=-1 wdt_test=no_user_checkin rd=mmcroot0 mmcrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1 vincefix=bluetool vincetouch=0 vincekeys=1 daisytouch=1 daisytouch_probe=1 daisytouch_gpio=1 daisytouch_power=1 startup_debug=0 -disable_kprintf daisylog=1 daisytouch_reports=1 daisytouch_trace=1 daisytouch_busywait=1' \
tools/build_daisy.sh artifacts/kernel-0193/kernel.development.vince artifacts/daisy-touch-busywait-0193
fastboot -s 6790a29e0305 boot artifacts/daisy-touch-busywait-0193/daisy-boot.img
```

- Kernel SHA-256: `859f9ac987af498ffc13a564b0606a41a9b2403600aecc905f3c74b354dda697`.
- Image SHA-256: `29506ebc17b44c188a1ffd2da24f0ab4e8a9e8839eae5cbc1eaae71bf0fe62a9`.
- Image size: 17,928,192 bytes.
- Build log: `artifacts/daisy-touch-dev/build-image-busywait-0193.log`.

Validated the boot header/hash, appended Daisy DTBs, empty `second`, complete
payload/ADT, RAM bounds, every kernel segment, exact busy-wait command line,
and linked delay-comparison/trace/power/identity/logger markers. After the user
confirmed readiness, rebooted TWRP to fastboot, confirmed `product: daisy`, and
temporarily booted serial `6790a29e0305` (success in **0.541 s**).

**Hardware result pending.** Requested one-/two-finger holds, slow swipes and
full releases during the first five minutes. Save the next extraction as
`artifacts/console-busywait-0193.log`. Compare `delay5 legacy` / `delay5 busy`
measurements, complete-transfer and byte-gap timings, IRQ samples and contact
validity against the 0192 touch trace. The test emits no touch HID events.
