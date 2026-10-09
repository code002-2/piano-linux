# Piano touch bring-up, iteration 2 (2026-09-26)

Follows `docs/touch-spi-trial.md`. Developed on `piano/touch-bringup` (umbrella, linux-piano) and `bp/touch-bringup` (debian-piano), then merged into the test-image lines (umbrella `main`, linux-piano `piano/test-bringup`, debian-piano `main`); tag `milestone-touch`. Build: `scripts/build-test-image.sh --jobs 32` → `debian-piano/out/test-image/{boot.img,dtbo.img,MANIFEST.txt}` (the only image set; the arm64 userland lives in umbrella `out/arm64-tools`).

## What the hardware needs (from the MiCode piano sources)

| Item | Source | Value |
|---|---|---|
| Touch IC | `vendor_xiaomi_proprietary_touch-driver/p81/nt36532` | Novatek NT36532(E) TDDI, host-download (no flash) |
| Driver mode | `p81/Android.mk` | `CONFIG_TOUCH_THP_SUPPORT=1`, `CONFIG_TOUCH_TDDI_SUPPORT=1`: the IC streams raw frames, coordinates are computed on the host |
| Bus | `piano-xiaomi-touch-pinctrl.dtsi`, `sun-pinctrl.dtsi` | QUP1 SE2 SPI, GPIO40-43 (`qup1_se2`, 6 mA, no bias), 19.2 MHz, mode 0 |
| IRQ | same | GPIO162, rising edge (`INT_TRIGGER_TYPE`) |
| Panel ID | `nt36xxx.c` `nvt_parse_dt()` | GPIO100 input: 0 = CSOT, 1 = BOE; selects the firmware |
| Reset GPIO | `nt36xxx.h` | none (`NVT_TOUCH_SUPPORT_HW_RST 0`) |
| Power | touch node has no supply | TDDI: powered with the panel. `touch_avdd_vreg` (GPIO114, also a CCI pin) is an unused leftover |
| Memory map | `nt36xxx_mem_map.h` (THP) | cascade chip: event buffer 0x11C400, polling info 0x1093D8 |
| Frame | `nt36xxx.c` `nvt_ts_work_func()` | read 256 event bytes + `frame_len` from the polling info; payload type @56, cols/rows @48/49, matrix @64 |
| Secure GPIOs | `sun.dtsi` `qcom,gpios-reserved` | 36-39, 48-51, 74 |

Both firmware blobs (`novatek_nt36532_piano_fw_{csot,boe}.bin`, fw 0x12 / 0x13) parse identically with the MiCode and the sheng-derived header parsers; the header CRCs equal the computed ILM/DLM CRC32.

## Changes

Kernel (`drivers/input/touchscreen/nt36532e`): THP memory map and frame length from the polling info, firmware picked by the panel ID pin, no-DRM-panel mode (display left on by the bootloader), IRQ enabled only after a successful download, full frame read on every IRQ, stock CS timing, sheng-only doze tuning removed, `/proc/nvt_thp_status` extended, `/proc/nvt_thp_cmd` for single extended host commands. Config: built-in pstore RAM + console, uinput.

Overlay (`debian-piano/boot/dtbo-piano-touch-v2.dts`): mainline TLMM node with the secure pins reserved and the SE2 pin state; stock `novatek@0` disabled (its `panel` phandle blocks probing and cannot be deleted by an overlay) and a new `touchscreen@0`; ramoops trades pmsg for oops records. Checked with `fdtoverlay` on the stock vendor DTB `vbdtb-04` (SunP v2 Alt. Thermal).

Probable cause of the earlier TLMM reboot: without `gpio-reserved-ranges`, gpiolib reads the direction register of every pin at registration, including the secure ones. With the stock reserved list the probe reads exactly the pins the stock kernel reads.

## Device procedure

Host, device in fastboot:

```
fastboot getvar current-slot        # must be b
fastboot flash dtbo_b debian-piano/out/test-image/dtbo.img
fastboot boot debian-piano/out/test-image/boot.img
nc 10.42.0.2 23                     # busybox telnetd
```

Device: `piano-touch-test --stage 0` … `--stage 4` one at a time (or no argument for all). Evidence: `http://10.42.0.2:8080/touch-test.log`, `/fdt.dtb`, `/tlmm-pre.txt`, `/pstore-prev/`.

If the device resets: hold Volume Down to land in fastboot directly (Android must not boot, or it overwrites ramoops), RAM-boot the same image again and read `/run/pstore-prev/console-ramoops-0`.

Rollback: the milestone-1 pair and the first tested touch pair are kept in umbrella `out/archive/` (`milestone-1-image/`, `touch-v2-tested/`).

## Results (device session 2026-09-26)

Image: kernel `7.2.6-00031-g7ec7e9a4bf12`, touch-v2 dtbo.

| Stage | Result |
|---|---|
| 1 TLMM probe (`pinctrl-sm8750`) | **passes** with `gpio-reserved-ranges`; no reset, pin state unchanged. The earlier reboot was the missing secure-GPIO list |
| 2 GPI + GENI SPI | binds; GPIO40-43 switch to `qup1_se2`, 6 mA; SE2 runs in GPI DMA mode (dma1chan0/1) |
| 3 first SPI transfer | **reset the SoC** — see below. With the SMMU fix: chip ID `0e 00 04 32 65 03` (NT36532, cascade), lcd-id 1 → **BOE** panel, `fw_boe.bin` (0x13) downloaded in 101 ms, poll info frame length 5192, 60x40 sensor, PID 0x59B0 |
| 4 THP frames | ~120 frames/s, type 3 (60x40 mutual), all checksums valid, no read errors; idle max delta ≈ 15, finger ≈ 1000-1300 |
| 5 uinput | two fingers tracked in separate slots; corners map to (125,185) and (3117,1994) on the 3200x2136 console |

**SMMU root cause.** The stock apps SMMU (`qcom,qsmmu-v500`, 0x15000000) has no mainline driver in this setup, so it stays as ABL left it: enabled, `sCR0.USFCFG=1` (unmatched streams fault), stream matches only for ABL's own masters (0x60, 0x540, 0x800/2, USB 0x40, 0x480), each routed to a context bank with stage 1 off. QUP1 GPI DMA (stream 0xb6) had no match; its first transfer faulted and the SoC reset (no log survives: the reset is below the kernel). `piano-qup-smmu` adds matches for 0xb6 and 0xa3 routed like USB. This is a bring-up workaround; the proper fix is a mainline SMMU binding in the overlay (`qcom,sm8750-smmu-500`) with correct `iommus` for every DMA master, handled with the display handoff streams in mind.

**Orientation.** Landscape console: sensor column → screen x, sensor row → screen y reversed (default in `piano-touch-view`).

pstore did not keep a log across these resets (the region was empty after reboot), so it cannot be relied on for SoC-level resets.
