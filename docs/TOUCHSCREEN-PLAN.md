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

Add the next board patch, provisionally `0184-daisy-ft5446-touch.patch`, after
the working 0183 baseline. Extend `vince_input.c` with an explicit FT5446
selection and reuse the QUP/TLMM helpers. Keep `MACHINE_CONFIG=VINCE`.

Proposed new options, **not implemented yet**:

- `daisytouch=1`: select FT5446 input; absent/zero keeps Daisy touch disabled.
- `daisytouch_probe=1`: with `daisytouch=1`, initialize and report identity
  without sending finger events.

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

Implement stage 1 and the shared I²C register-read transport, build one
probe-only CI kernel, and verify `0x5422` in XNU. That establishes the
power/reset/bus foundation before report decoding and iOS gesture debugging.
