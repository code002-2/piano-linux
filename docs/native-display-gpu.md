# Native display and GPU bring-up (msm + NT36532 + Adreno 830)

Status: implemented and built offline; not yet validated on the device. This document describes the design, the component changes and the staged device test.

## 1. Hardware facts

| Item | Value |
|---|---|
| Panel | 3200x2136 LCD, video mode, two DSI links (4 lanes each), DSC 1.1 (800x24 slices, 10 bpc, 8 bpp), RGB101010, NT36532 TDDI |
| Panel variants | "p81 42 02 0a" = BOE (Xiaomi vendor byte 0x42), "p81 35 02 0b" = CSOT. The touch controller's lcd-id pin reads 1 = BOE on the test device, so the BOE sequence is the default |
| Link | fixed 1197.5 Mbps per lane; the refresh rate is chosen with the horizontal front porch (HFP per link 21 / 117 / 309 for 144 / 120 / 90 Hz), VFP 68, VSYNC 2, VBP 104 |
| Panel rate control | page 0x10, registers 0xb2/0xb3: 144 Hz `00/00`, 120 Hz `91/40`, 90 Hz `00/80` |
| Reset | TLMM 98, active low |
| Supplies | vddio = L12B (never written by HLOS), LCD bias +5.8 V / -5.8 V enabled by TLMM 117 / 118 |
| Backlight | two KTZ8866 at 0x11: I2C hub SE3 (HWEN = TLMM 2) and QUP2 SE8, always programmed identically; 11-bit brightness, 6 sinks (enable value 0x7f) |
| GPU | Adreno 830, chip id 0x44050001, GMU at 0x3d6c000, Adreno SMMU at 0x3da0000, zap shader region at 0x9b09a000 |

With RGB101010 input and 8 bpp DSC, the msm DSI host derives exactly 1197.5 Mbps per lane from the downstream porches for all three native rates. The 60 Hz mode reuses the 120 Hz timing with a doubled vertical total, as the downstream dynamic refresh rate code does.

## 2. Design

The kernel only ever sees the stock vendor DTB plus our dtbo_b overlay. Everything for the native display and the GPU is added as new mainline nodes next to the stock ones (`debian-piano/boot/dtbo-piano-display.dts`); the stock SMMU, dispcc, gpucc, DSI and KGSL nodes stay driverless.

- **Apps SMMU.** msm refuses to run without an IOMMU, so the apps SMMU (0x15000000) gets a second, mainline node that only the MDSS references. The qcom SMMU driver adopts every stream match that is valid when it probes as bypass, so it is a module loaded after `/pianoinit` has installed the stream matches of the running masters. Touch, Bluetooth and GPI keep referencing the stock node and cannot be attached to the new driver. Nothing may add stream matches after the driver is bound.
- **Power.** DSI/PHY and panel I/O rails are fixed-regulator stand-ins (writing those RPMh resources during scanout reset the SoC in earlier rounds). MMCX comes from a mainline rpmhpd instance. Until its `sync_state()`, rpmhpd clamps every domain in use to its highest level and never votes anything down. A consumer node without a driver (`rpmhpd-hold-ml`) keeps `sync_state()` from happening under the default strict fw_devlink policy. Cost: MMCX and CX stay at their top level while the display is up. The GPU gets no rpmhpd domains: the GMU votes GX itself, and the a8xx driver uses the GX power domain only for recovery.
- **Interconnects.** The MDSS has no interconnect paths; the multimedia NoC provider stays unbound, so the bootloader's votes remain in place.
- **Backlight.** The primary KTZ8866 mirrors every register write to the secondary chip (`kinetic,secondary-backlight` / `kinetic,secondary`), and takes over the bootloader brightness instead of jumping to a default.
- **Decoupling.** msm runs with `separate_gpu_kms=1` by default, so the GPU is a DRM device of its own and a GPU failure cannot hold back the display.
- **Loading.** All drivers of the new nodes are modules that only `piano-display` (`/usr/lib/piano/display-start`) loads, in stages. The service is opt-in (`/etc/piano/native-display`) until the device test passes.

## 3. Components

| Repository | Branch | Content |
|---|---|---|
| linux-piano | `piano/display-v1` | backports: DSC slices per packet (drm-misc-next ffa88de8ddb6, ce73a5db44e3), NT36532 panel driver and binding (drm-misc-next 1c523f0a9301, 47b823940e38); piano: NT36532 mode tables and ordered supplies, Xiaomi Pad 8 Pro BOE/CSOT panels, KTZ8866 secondary chip and bootloader brightness, defconfig, rootfs profile (msm and arm-smmu as modules) |
| linux-piano | `bp/a830-v1` | backports: "drm/msm: Support for Adreno 830 GPU" (all six patches), sm8750 GPU clock and IOMMU nodes (qcom f695831a7af1), the remaining two patches of the A8x GX GDSC series |
| linux-piano | `piano/integration-display-gpu` | both of the above, for image builds |
| piano-firmware | `gpu/a830-firmware` | `qcom/gen80000_{sqe.fw,gmu.bin,aqe.fw}` from linux-firmware, device-signed zap shader `qcom/sm8750/xiaomi/piano/gen80000_zap.mbn` |
| debian-piano | `display/native-display` | display/GPU overlay, `display-start`, `piano-display.service`, `/etc/piano/display.conf`, module blacklist, Mesa rebuild (`scripts/build-mesa-debs.sh`, `.github/workflows/mesa.yml`) |

The panel init sequences were generated from the downstream panel descriptions and compared byte for byte with them for 144, 120 and 90 Hz.

## 4. Mesa

No released Mesa recognises the A830 on drm/msm: the kernel reports chip id 0xffff44050001 (speed bin in the upper half), which only Mesa main lists (commit 2061a5ee, MR !43878). `scripts/build-mesa-debs.sh` rebuilds the trixie-backports source package (26.1.6) with that one-line upstream patch; the `mesa` workflow runs it natively on an arm64 runner. Until those packages are in the image, `/etc/environment` keeps forcing llvmpipe.

## 5. Staged device test

Each stage adds one new kind of hardware access. Stream the kernel log from the host during every stage (`ssh piano@10.42.0.2 sudo cat /proc/kmsg`); every stage writes `piano-display: stage N ... begin/done` markers.

Preparation (device running the current GNOME image, USB NCM up):

1. Copy the kit's module tree, firmware and overlay files to the device and run `depmod` for the new kernel release (see the kit's README).
2. `fastboot getvar current-slot` must report `b`. Flash only `dtbo_b` with the kit's dtbo.img, then RAM boot the kit's boot.img (`fastboot boot boot.img`). Always use the dtbo.img and boot.img of the same kit.
3. After boot, check that SSH, touch and radios behave as before: the new nodes are inert until the stages run.

| Stage | Command | Expected | Stop if |
|---|---|---|---|
| 1 smmu | `sudo /usr/lib/piano/display-start --stage 1` | `arm-smmu 15000000.iommu` probes; display, SSH, touch unchanged | link drops, reset, display garbage |
| 2 gpu-clk | `--stage 2` | gpucc and `3da0000.iommu` bound | reset |
| 3 msm | `--stage 3` | "Loaded GMU firmware", zap shader loaded, `/dev/dri/renderD128`; simpledrm and GNOME unchanged | GMU timeout, SMMU faults, reset |
| 4 backlight | `--stage 4` | `/sys/class/backlight/ktz8866-backlight`, brightness equals the bootloader level; writing `brightness` changes it | I2C errors, backlight off |
| 5 display | `sudo systemctl stop gdm` then `--stage 5` | brief black screen, then the console on the native 144 Hz mode; `/sys/class/drm/card*-DSI-1/modes` lists 144/120/90/60 | black screen with the backlight on, DSI errors, reset |

After stage 5: `modetest -M msm -c` (modes), switch rates with `modetest -M msm -s <connector>@<crtc>:3200x2136-120`, check brightness from GNOME once gdm is started again. Enable the service with `sudo touch /etc/piano/native-display` only after all stages passed.

Rollback: reboot. The overlay is inert without the stages, and slot A stays stock.

## 6. Open items

- DPMS / screen blanking: the LCD bias rails are still always-on; turning them off in panel unprepare needs a device test.
- ADSP coexistence: now that the panel can be re-initialized, check whether the display survives an ADSP boot.
- rpmhpd: describe every rail consumer, then drop the hold node so MMCX/CX can scale down.
- GPU: GX GDSC handling for recovery (gxclkctl), devfreq and thermal limits.
- Seamless refresh rate switching (without a full modeset).
