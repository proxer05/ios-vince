# ios-daisy

Daisy (Xiaomi Mi A2 Lite, Qualcomm MSM8953) port of the `ios-vince` XNU-8796 / iOS 16
bring-up. This is currently the **no-touch first-boot branch**: the vince kernel, shim,
root filesystem and iPhone X ADT are reused, with the daisy panel geometry substituted.
Touch is deliberately disabled.

## Target Hardware
- **SoC:** Qualcomm Snapdragon 625 (MSM8953), 8x ARM Cortex-A53 @ 2.0 GHz
- **Display:** HX8399C FHD+ video-mode DSI panel, 1080x2280, 24-bit RGB888, stride 3240 bytes, continuous-splash at `0x90001000`
- **Interrupt Controller:** ARM GICv2 MMIO (Distributor: `0x0b000000`, CPU Interface: `0x0b002000`)
- **Timer:** ARM Generic Virtual Timer (PPI 20 / INTID 20)
- **Storage:** Qualcomm SDHCI eMMC 5.1 (`0x07824900`), GPT partition named `userdata`
- **Touch (not enabled yet):** two daisy variants: Goodix GT9xx on I2C-3 at `0x5d`, or
  FocalTech FT5446/FT5435 on I2C-3 at `0x38`; both use reset GPIO 64 and IRQ GPIO 65
- **RAM:** 4 GB variant; the shim uses the 1.5 GB upper DRAM bank at `0xa0000000`

## What Changed From Vince
- Framebuffer height 2160 -> 2280 in the shim console, ADT and early debug console.
- The XNU framebuffer driver still receives `vWidth/vHeight/vRowBytes` through the boot
  arguments, so the 24-bpp 1080x2280 panel path is otherwise unchanged.
- Touch is optional at this stage and is not on the boot path. The vince Synaptics RMI4
  driver is compiled but disabled by default in this branch (`vincetouch=0`); daisy uses a
  Goodix GT9xx or FocalTech FT5446/FT5435, so the vince probe would only find no device.

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

The script's payload cmdline defaults to `rd=mmcroot0 mmcrw=1 vincetouch=0 vincekeys=1`:
root from the `userdata` GPT partition, writes enabled, touch off, volume/power keys on.
Change `PAYLOAD_CMDLINE` in the script if you want a different console or debug level.

The iOS root is installed into the `userdata` GPT partition, so this erases Android user
data. Keep a backup.

## Not Yet Done
- daisy touch driver (Goodix GT9xx at `0x5d` or FocalTech FT5446/FT5435 at `0x38`,
  I2C-3, reset GPIO 64, IRQ GPIO 65)
- daisy-specific GPU zap/secure-memory address verification (same Adreno 506; CPU
  rendering is enough for first boot)
- daisy WLAN/BT calibration and MAC handling
- audio, camera, sensors, charger/battery polish
- renaming the internal board config from `VINCE` to `DAISY`
