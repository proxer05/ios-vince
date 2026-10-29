# ios-daisy

Daisy (Xiaomi Mi A2 Lite, Qualcomm MSM8953) port of the `ios-vince` XNU-8796 / iOS 16
bring-up. This is currently the **no-touch first-boot branch**: the vince kernel, shim,
root filesystem and iPhone X ADT are reused, with the daisy panel geometry substituted.
Touch is deliberately disabled.

**Status (2026-10-29):** iOS 16.7.16 **homescreen confirmed** on the 3 GB / 64 GB
daisy with the artwork ADT fixes, stale MobileGestalt cache invalidated, and
`VINCE_NO_FBCON=1`. The `sweethidtest=30` boot option supplies simulated Home
presses to get past the lockscreen with touch disabled. The no-touch homescreen
milestone is complete. See [boot diagnostics](docs/BOOT-DEBUG.md) for the
known-working image and build command.

**Touch identification:** TWRP confirmed **FocalTech FT5446** (`0x5422`),
Ofilm touch panel + CSOT LCD, firmware `0x0c` on this handset. XNU's opt-in
0190 test now confirms RPM L10/L6 ACKs, ready rails, and three successful GPIO
identity reads. Coherent reports and touch HID delivery are next. See the
[touchscreen bring-up plan](docs/TOUCHSCREEN-PLAN.md).

## Target Hardware
- **SoC:** Qualcomm Snapdragon 625 (MSM8953), 8x ARM Cortex-A53 @ 2.0 GHz
- **Display:** HX8399C FHD+ video-mode DSI panel, 1080x2280, 24-bit RGB888, stride 3240 bytes, continuous-splash at `0x90001000`
- **Interrupt Controller:** ARM GICv2 MMIO (Distributor: `0x0b000000`, CPU Interface: `0x0b002000`)
- **Timer:** ARM Generic Virtual Timer (PPI 20 / INTID 20)
- **Storage:** Qualcomm SDHCI eMMC 5.1 (`0x07824900`), GPT partition named `userdata`
- **Touch (identified, not enabled in XNU yet):** FocalTech FT5446 on I2C-3 at
  `0x38`, reset GPIO 64, IRQ GPIO 65; 10 slots, 1080x2280 coordinate range.
  Other daisy variants can use Goodix GT9xx at `0x5d` or FocalTech FT5435.
- **RAM:** tested 3 GB variant; XNU uses the upper-memory subset starting at
  `0xa0000000`, size `0x57f00000` (with a separate 1 MiB panic-log reservation)

## What Changed From Vince
- Framebuffer height 2160 -> 2280 in the shim console, ADT and early debug console.
- The XNU framebuffer driver still receives `vWidth/vHeight/vRowBytes` through the boot
  arguments, so the 24-bpp 1080x2280 panel path is otherwise unchanged.
- Touch is optional at this stage and is not on the boot path. The vince Synaptics RMI4
  driver is compiled but disabled by default in this branch (`vincetouch=0`);
  this daisy handset needs an FT5446 protocol handler at address `0x38`.

The machine config is intentionally still named `VINCE` for the first boot. The MSM8953
GICv2/timer/eMMC/PMIC/PSCI code is identical on daisy; renaming the board config can wait
until the first boot is confirmed.

## First Boot
Use the existing vince XNU build/payload/root-image tooling, with the daisy ADT and shim:

1. Build the XNU kernel with `MACHINE_CONFIG=VINCE` (the same MSM8953 config).
2. Put the resulting `kernel.development.vince` in a path you control.
3. Run `tools/build_daisy.sh KERNEL [OUTDIR]`. It generates the 1080x2280 ADT, builds
   the shim, packages the `VINCEXNU` payload, and writes `OUTDIR/daisy-boot.img`.
4. `fastboot boot OUTDIR/daisy-boot.img` and watch the early console on the panel.

The script's payload cmdline defaults to `rd=mmcroot0 mmcrw=1 rootrw=1
unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 ignition_level=1
vincefix=bluetool vincetouch=0 vincekeys=1`: root from the `userdata` GPT partition,
writes enabled, iOS binaries allowed, touch off, volume/power keys on. Change
`PAYLOAD_CMDLINE` in the script if you want a different console or debug level.

The iOS root is installed into the `userdata` GPT partition, so this erases Android user
data. Keep a backup.

## Not Yet Done
- daisy FT5446 touch driver: see the [staged plan](docs/TOUCHSCREEN-PLAN.md)
- daisy-specific GPU zap/secure-memory address verification (same Adreno 506; CPU
  rendering is enough for first boot)
- daisy WLAN/BT calibration and MAC handling
- audio, camera, sensors, charger/battery polish
- renaming the internal board config from `VINCE` to `DAISY`
