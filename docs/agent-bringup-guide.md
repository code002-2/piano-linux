# Bring-up guide for agents (piano, after milestone-touch)

Audience: an agent (possibly a smaller model) continuing the Xiaomi Pad 8 Pro (piano, SM8750) port. Read this completely before touching code or the device, together with `AGENTS.md`, `docs/device-bringup-runbook.md` §7-§8 and `docs/touch-bringup-v2.md`. Everything here was measured on the device unless marked *(unverified)*.

## 1. How this device actually boots (do not re-derive)

- RAM boot only: `fastboot boot boot.img` after `fastboot flash dtbo_b dtbo.img`, `current-slot` must be `b`. Never write anything else (AGENTS.md rule 5).
- ABL composes the **stock vendor DTB** (vendor_boot, `vbdtb-04` = "SunP v2 Alt. Thermal Profile") **plus our dtbo_b**. Our kernel never sees a mainline DTB. Every mainline node is an overlay fragment in `debian-piano/boot/dtbo-piano-touch-v2.dts` (which `#include`s the milestone-1 base `dtbo-piano-usb-nopd9.dts`).
- `/soc` in the stock tree is **1-cell address / 1-cell size**. Mainline `reg = <0x0 A 0x0 S>` must be written `reg = <A S>`.
- Overlay fragments: use `target-path` or `target = <&stock_label>`; new fragment numbers above the highest existing one (currently 228); labels you add yourself are fine inside the overlay. ABL rewrites phandles.
- You can simulate exactly what ABL builds: `fdtoverlay -i vbdtb-04.dtb -o merged.dtb overlay.dtb` (`vbdtb-04.dtb` extracted from the stock vendor_boot) (extract `overlay.dtb` from the DTBO container: header 32 B + entry 32 B, entry holds size/offset big-endian). For the milestone-1 overlay this was verified byte-identical to the live `/sys/firmware/fdt`. Always check the merged tree before a device test.
- Display is `simpledrm` on the bootloader's splash framebuffer (3200x2136, stride 12800, a8b8g8r8). No native DRM/DSI driver runs; the panel is powered by ABL and must be left alone.

## 2. Things that reset the SoC (each cost a device round)

| Trigger | Symptom | Rule |
|---|---|---|
| Unmatched **apps SMMU** stream (any new DMA master) | silent reset on first DMA, no log anywhere | see §3; check before loading any DMA driver |
| TLMM read outside 0x0f100000..0x0f202000, or of a secure GPIO | reset within seconds | TLMM node needs `gpio-reserved-ranges = <36 4>, <48 4>, <74 1>` |
| Reprogramming panel RPMh rails (L12B/L9B) or voltages mid-scanout | panel garbage + reset | never touch display rails |
| Booting the ADSP firmware | panel dies permanently (backlight stays) | ADSP work needs a separate display strategy |
| M31 eUSB2 PHY init | bus hang | USB2 stays on `usb_nop_phy` |
| Leaving custom `boot_b`/`vbmeta_b` next to stock | ABL aborts before the kernel | only dtbo_b is ever flashed |

pstore/ramoops is built in and the console is mirrored into it, but on this device **it did not survive any of these resets**. Do not plan around pstore. What works: `/dev/kmsg` streamed live to the host over telnet (`cat /proc/kmsg`) — the last line before the link drops is your crash point. Write a marker to `/dev/kmsg` before every risky step.

## 3. The apps SMMU (read this before any DMA peripheral)

The stock SMMU node is `qcom,qsmmu-v500` at 0x15000000; nothing binds it, so it keeps the ABL state: `sCR0 = 0x002D0406` (USFCFG = 1, unmatched streams fault), 127 stream-match groups, matches only for streams ABL uses:

| slot | stream/mask | owner | context bank |
|---|---|---|---|
| 0 | 0x60 | UFS | 0 |
| 1 | 0x540 | SDHC | 1 |
| 2 | 0x800/0x2 | display (0x800, 0x801) | 2 |
| 3 | 0x40 | USB dwc3 | 3 |
| 4 | 0x480 | ? | 4 |

All those context banks have stage 1 off (pass-through). `piano-qup-smmu` (runs at boot from init) adds 0xb6 (QUP1 GPI) and 0xa3 (QUP1 SE) routed like USB. Stream IDs of the other masters, from the stock `iommus`:

| Master | stream(s) | notes |
|---|---|---|
| QUP1 GPI / SE (touch SPI, keyboard i2c SE6) | 0xb6 / 0xa3 | matched at boot |
| QUP2 GPI / SE (uart14 BT, i2c on 0x8c0000) | 0x436 / 0x423 | **not matched** — add before using QUP2 DMA |
| PCIe0 (WLAN) | 0x1400, 0x1401 (`iommu-map`) | not matched |
| audio (q6apm-dai buffers) | 0x1001/0x80, 0x1041/0x20 | matched at boot (dais `iommus`) |
| video (vidc) | 0x1940.. | not matched |
| GPU | separate KGSL SMMU 0x3da0000 | own SMMU, same issue class |

To extend: `piano-qup-smmu 0x436 0x423` (script accepts IDs). Verify with `piano-qup-smmu --check`. The long-term fix is a mainline `qcom,sm8750-smmu-500` node with proper `iommus` everywhere, designed so the display streams keep working (the kernel would reset all SMRs at probe). Do not attempt that without a plan for the display handoff.

## 4. Working method (what made touch succeed in one session)

1. **Read the vendor source first.** Xiaomi's MiCode kernel release for piano holds the exact kernel, DT and drivers shipped on the device. For a driver, find its build flags (`Android.mk`, `Kbuild`, `*.conf`) — e.g. touch is built with `CONFIG_TOUCH_THP_SUPPORT=1`, which changes the memory map. Other Xiaomi devices (sheng, p82) are only hints.
2. **Validate data offline.** Firmware headers, overlays (fdtoverlay), userspace tools (static musl + `qemu-aarch64-static` with synthetic input) — before any device round.
3. **Stage device work** so each step adds exactly one new hardware access, read-only first. Pattern: `piano-touch-test` (stages 0-4).
4. **Use `devmem` read-only** to learn hardware state (pin mux, SMMU tables, clocks) — always bound-check addresses on the host first.
5. **First data transfer through a trivial path** (`spidev` + a 4-byte read) before the real driver: it separates bus problems from driver problems.
6. **Every device round**: host log stream running, marker in kmsg, one change, and a written expectation of the outcome.
7. Build only with `scripts/build-test-image.sh --jobs "$(nproc)"`. It regenerates the kernel release and checks module vermagic (stale vermagic once masked five "independent" bugs). Never run `make` in a way that can stop at a Kconfig question in the background: after changing `.config` run `make olddefconfig`.

## 5. Tooling on the test image

- Host: `nmcli connection up piano-ncm` (10.42.0.1/24, matched by MAC 02:66:77:88:99:aa). Device: telnet 10.42.0.2:23, HTTP of `/run` on :8080.
- Host has no `nc`; drive telnet from Python (answer the busybox `ESC[6n` cursor query).
- Serve files to the device with `python3 -m http.server --bind 10.42.0.1` and `wget` on the device; load modules with `insmod`/`modprobe`.
- `piano-tests` menu, `piano-touch-test`, `piano-touch-view`, `piano-qup-smmu`, `piano-collect`.

## 6. Remaining work, in recommended order

Each item: goal, vendor reference, known facts, first safe step, risks.

### 6.1 Touch → desktop input (small)
- Done: THP frames, simple tracker, uinput (`piano-touch-test --stage 5`).
- Next: package a proper THP service. Upstream candidate `ianchb/xiaomi-sheng-thp` (Apache-2.0, sheng) expects the same `/proc/nvt_thp_*` interface; piano differs in frame length (5192 vs 5160, read from poll info) and orientation (rows reversed). Fork per AGENTS.md rule 4 into `userspace/`, keep the diff data-only (a profile).
- Stylus: frames of type 6/7/9/0x1d appear once `/proc/nvt_thp_stylus` is enabled; pen pressure comes over Bluetooth (needs BT first).

### 6.2 Keyboard + touchpad (medium)
- Stock: `nanosic,803` at i2c 0x4c on QUP1 SE6 (`qupv3_se6_i2c`, i2c@a98000), IRQ gpio97, reset gpio188, status gpio95, sleep gpio3, supplies dvdd/vdd; driver in the MiCode piano sources (search `nanosic`), sheng precedent `ianchb/xiaomi-sheng-keyboard-helper`.
- QUP1 streams are already matched. Needs: mainline `qcom,geni-i2c` binding for SE6 (same conversion as SE2 in the touch overlay), pins via the mainline TLMM node, the driver port.
- Risk: the supplies are PMIC regulators — only read their state; do not enable/adjust rails until you know they are off-panel.

### 6.3 Battery / charging (MVP, hard)
- ADSP runs from the `piano-adsp` service; with the native display path holding its power domains, starting it no longer blanks the panel.
- Battery: two TI bq27z561 gauges (one per parallel cell) on QUP1 SE5 and QUP2 SE3, driven by mainline `bq27xxx` through the subsystem overlay. The capacity shown matches stock Android.
- The ADSP firmware speaks Xiaomi's MCA property protocol on the battmgr glink owner, not Qualcomm battmgr. `piano_mca` (linux-piano) replaces `qcom_battmgr`, issues reads only, and reports the USB input as a `power_supply` (online when the bus voltage is above 4 V) *(USB input: unverified on the device)*.
- Charging works at the ADSP/PMIC default limits. Do not write charger properties. The Xiaomi fast-charge protocol (MiPPS) is a separate, later item.

### 6.4 WLAN / BT (medium)
- WLAN = "peach" PCI 17cb:110e (ath12k, WCN7850 path, commit on the kernel line). PCIe needs the SMMU streams 0x1400/0x1401 matched first.
- BT = uart14 on QUP2 (streams 0x436/0x423) + pwrseq-qcom-wcn.

### 6.5 Native display (DRM/DSI) and GPU (hard, MVP)
- Needed for the GPU and for real power management of the panel.
- Panel: the stock tree offers two Xiaomi P81 LCD panels (`dsi_p81_42_02_0a_dualdsi_dsc_lcd_video`, `..._35_02_0b_...`, dual-DSI DSC video) besides the NT37801 AMOLED that the early mainline DTS guessed; the 3200x2136 dual-DSI splash matches the P81 LCDs *(which of the two: unverified; the touch lcd-id pin read 1 = BOE)*. A panel driver must come from the MiCode display sources (`vendor_opensource_display-drivers`, `vendor_qcom_opensource_display-devicetree`).
- Display streams 0x800/0x801 are already matched by ABL.
- GPU: `bp/gpu-v1` (XEC A830 backport) exists but is untested; the KGSL SMMU (0x3da0000) has the same unmatched-stream issue.

### 6.6 Sensors, suspend, rest
- Sensors work through the ADSP sensors hub (SSC): mainline `fastrpc` binds the stock node, and the userspace stack (Qualcomm's `adsprpcd` serving the sensors PD, libssc, a patched iio-sensor-proxy, the first-boot read-only import of the per-device sensor configuration from odm and registry from persist) comes as Debian packages from the `piano-sensors` repository. The sensors PD publishes its sensors one by one after `adsprpcd` attaches, and iio-sensor-proxy looks each up only once, so the `adsprpcd-sensorspd` unit counts as started only once libssc finds all of them (otherwise the desktop gets the accelerometer but no light sensor). The accelerometer mount matrix is `0, 1, 0; -1, 0, 0; 0, 0, 1`; GNOME auto-rotate works in all four orientations *(packaged build: unverified on the device)*.
- Hall sensors: only the keyboard lid hall (gpio80, `SW_LID`) is exposed. The second hall (gpio11) is left out because a `SW_TABLET_MODE` switch stuck at 0 keeps mutter out of touch mode, which disables auto-rotate.
- Audio runs on the ADSP AudioReach stack (q6apm/q6prm on the downstream GPR client ports 3 and 7). Microphones: the VA macro digital microphones over codec DMA. Speakers: four FourSemi FS19xx amplifiers on I2C hub SE0, fed by the secondary LPAIF TDM port with 4 x 32-bit slots at 48 kHz. Seen in landscape with the front camera at the top (the vendor tree names them for portrait): 0x35 top left, 0x37 top right, 0x34 bottom left, 0x36 bottom right, on TDM slots 0 to 3 in that order (fixed by the vendor preset `fs19xx.fsm`). The card gives the slots the channel types FL, FR, LS, RS, so a stereo stream plays on the top pair and a four-channel stream on all four. The amplifiers need about 2.5 ms after their shutdown pin is released before they answer on I2C, and longer on their first power-up after a cold boot, so the driver polls the chip id for up to 100 ms. The kernel line carries the TDM endpoint support, the FS19xx driver and a `xiaomi,piano-sndcard` machine entry; the topology is built from debian-piano `topology/`. The first capture stream used to reset the SoC: q6apm-dai tags PCM buffer addresses with a stream id taken from the `iommus` of its `dais` node, and without one the ADSP used 0x1000, which no SMR matches; the overlay now gives the node the stock audio streams 0x1001/0x80 and 0x1041/0x20. `piano-audio` loads the chain after the ADSP and a UCM profile gives PipeWire the Speaker and Mic devices. Verified on the device: capture, speaker playback and the speaker layout (checked by ear). Unloading and reloading the card at runtime leaves capture broken (the ADSP does not answer the buffer unmap), so reboot instead.
- Suspend/resume needs the native display path first.

## 7. Before you finish a session

- Commit in English, one logical change per commit, on a feature branch.
- Record device results (with kernel release and image hashes from `MANIFEST.txt`) in a `docs/` file.
- Update the workspace session notes, which live outside every repository.
- Never leave the device with anything but stock partitions + dtbo_b.
