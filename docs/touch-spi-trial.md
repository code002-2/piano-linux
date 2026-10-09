# Piano touch SPI trial (2026-09-25)

> Historical record. Superseded by `docs/touch-bringup-v2.md`; the `build-adsp-test-image.sh` entry point and the `debian-piano/out/adsp-m1/` artifacts named below no longer exist (touch is part of `scripts/build-test-image.sh`).

This trial is on descendants of the `milestone-1` tag. The known-good tag and slot A were not modified. The trial builds from the workspace root with `./scripts/build-adsp-test-image.sh --jobs "$(nproc)"` and stages its images in `debian-piano/out/adsp-m1/`.

## Device result

- Confirmed `fastboot getvar current-slot` returned `b` before writing only `dtbo_b`; started the matching `boot.img` with `fastboot boot`.
- NCM at `10.42.0.2` came up and the framebuffer remained 3200 x 2136. The operator confirmed the screen was still lit after more than two minutes.
- After loading `gpi` and `spi-geni-qcom`, `a88000.spi` bound to `geni_spi` and `/sys/bus/spi/devices/spi0.0` appeared with modalias `spi:NVT-ts-spi`.
- `nt36532e_ts` loaded but did not bind. `devices_deferred` reports `spi0.0  spi: wait for supplier /soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_nt37801_wqhd_plus_vid`. `/proc/nvt_thp_status` is therefore absent; no touch frames have been read.
- The SCMI protocol `0x10` channel error still appears on the console. It did not prevent SPI controller registration; its wider impact is not resolved.

## Binding changes

The stock DT uses `qcom,spi-geni`, clock name `se-clk`, 5-cell GPI DMA, and 1-cell interconnect specifiers. The mainline driver and SM8750 DTS expect `qcom,geni-spi`, clock name `se`, 3-cell GPI DMA, and 2-cell interconnect specifiers. The isolated touch overlay maps these properties and enables the QUP1 wrapper, GPI DMA, and the interconnect providers required by SPI2. Sources: `linux-piano/arch/arm64/boot/dts/qcom/sm8750.dtsi`, `linux-piano/drivers/spi/spi-geni-qcom.c`, and the GPI/SPI DT bindings.

The downstream TLMM node at `0xf000000` does not match the mainline TLMM binding at `0xf100000`. The trial leaves GPIO and touch pin levels untouched and clears only SPI controller pinctrl references to isolate controller probe. It does not select a touch firmware image or enable the DRM panel driver. The next touch step requires a separately reviewed panel/pinctrl plan; the presence of `spi0.0` alone is not evidence of functional touch input.

The last on-device-tested image records `boot.img` SHA256 `de649e233b15c98ae014086e5014edb46529adebc713a094cb72cab215028112` and `dtbo.img` SHA256 `26fe3551ddf82187e11d3c56ad5a936e980ef6889e37f5741d5610b422078a7a`. The original milestone-1 image pair is retained locally under `debian-piano/out/test-image/` for slot-B rollback.

## Isolated TLMM trial and follow-up

Commit `0170576` added an independent mainline TLMM node at `0xf100000`, matching `linux-piano/arch/arm64/boot/dts/qcom/sm8750.dtsi`. The device showed backlight but no console after that boot attempt. The USB cable was also loose, so the missing host NCM interface is not conclusive evidence of a kernel crash. Fastboot later reported `slot-unbootable:b: no`, but `oem lkmsg` returned `FAILNo such section`; no kernel trace was recoverable. The trial cannot yet isolate a failure mechanism. The node overlaps the stock `qcom,sun-tlmm` resource (`0xf000000` + `0x202000`) and was removed in follow-up commit `0084cb1` to avoid running two TLMM providers over the same hardware.

The follow-up image booted with NCM reachability (3/3 ICMP replies); the operator confirmed normal console display, and the device remained reachable after approximately two minutes. `piano-touch-test` again found `a88000.spi` bound to `geni_spi` and `spi0.0` with modalias `spi:NVT-ts-spi`. Its `devices_deferred` output names the immediate touch blocker: `spi0.0` waits for the downstream DSI panel node. Separately, `soc:touch_avdd_vreg` cannot obtain GPIO114, but the stock touch node does not reference that regulator as a supply, so its effect on touch power is not established. The driver waits for `panel_on` before checking the chip ID. Forcing probe while the panel and GPIO providers are unresolved would reach chip I/O with unknown power state; that is not part of this trial.

## MiCode P81 touch source review

The piano device tree includes `refer/MiCode_piano/kernel_devicetree/qcom/piano-xiaomi-touch-pinctrl.dtsi`. Its touch node uses GPIO162 for IRQ, GPIO100 for panel identification, and GPIO40-43 for QUP1 SE2 SPI. The P81 driver in `refer/MiCode_piano/vendor_xiaomi_proprietary_touch-driver/p81/nt36532/` reads GPIO100 as an input: 0 selects the CSOT firmware, 1 selects BOE. It does not request the optional reset GPIO (`NVT_TOUCH_SUPPORT_HW_RST=0`). The P81 and P82 `nt36xxx.c` files are byte-identical in this reference snapshot.

The current port does not read GPIO100 or select either firmware, and the trial DTBO deliberately supplies no `firmware-name`. This prevents an unverified panel-family download. The stock touch `panel` property contains three candidate phandles, whereas `drm_panel_add_follower()` in this kernel resolves only index 0. None of those stock downstream panel nodes is a DRM panel in the running simpledrm system. A future panel integration must first identify the actual panel and give the follower exactly that panel reference.

In the ported driver, the engineering reset is an SPI write. Kernel commit `3182dd3f6` moves it after the DRM panel-prepared check and propagates follower registration errors. The 32-core build passed and produced boot SHA256 `f5153a424a708a39b56f1a063e2321d2393dbcfc3774a330ac759023bc7b0d14`; its DTBO is byte-identical to the last tested one. This is source-level safety work only: firmware selection remains disabled, and the new boot image has not been started on the device.

## Controlled TLMM node isolation (device result)

The next image set separates DTBO parsing from the mainline TLMM driver probe. Kernel commit `6b8e1fa55` makes `CONFIG_PINCTRL_SM8750=m`; the build stages `pinctrl-sm8750.ko` in the initramfs, but its init script does not load it. The DTBO in Debian commit `0d1f05c` restores the same independent `pinctrl@f100000` node as the failed trial without redirecting any stock GPIO consumer. Its SHA256 is exactly the earlier failed DTBO hash, `db34250860c4683338ec0765f9dbec94607302be458f8a4ca229260d0caf855f`. The matching RAM-boot image SHA256 is `03e45fff67a71179ad05b06a596c10c2e5c80f61b9d596151369cc4b67878ed7`.

The first stage passed: the operator confirmed sustained console display, NCM answered 3/3 pings, `f100000.pinctrl` was present without a driver, and `pinctrl_sm8750` was absent from `/sys/module`. The kernel release was `7.2.6-00028-g6b8e1fa55d83`.

The second stage failed immediately after `modprobe pinctrl-sm8750`: the telnet command never returned, the panel went black, NCM disappeared, and the operator reported a device reboot. The operator returned to fastboot; `current-slot` remained `b` and `slot-unbootable:b` was `no`. ABL returned `FAILNo such section` for both `oem lkmsg` and `oem lpmsg`. A subsequent RAM boot of the same image worked again with the module unloaded, but mounted pstore was empty. The exact fault within `msm_pinctrl_probe()` is unknown. The operator confirmed normal display again after this RAM boot. Do not reload this module on piano without a way to capture a crash trace. No touch firmware or GPIO output was selected during either stage.

Subsequent trial builds exclude `pinctrl-sm8750.ko` from the initramfs to prevent accidental repetition, and the touch test reports this known failure. The kernel configuration remains modular, preserving the isolation experiment in history. This packaging change does not resolve the touch dependencies.

Source review found a concrete omission in the new TLMM node: `refer/MiCode_piano/kernel_devicetree/qcom/sun.dtsi` declares `qcom,gpios-reserved = <36 37 38 39 74 48 49 50 51>`. The saved merged device tree has the same list. The new node now expresses it using the mainline binding's `gpio-reserved-ranges = <36 4>, <48 4>, <74 1>`. In `linux-piano/drivers/gpio/gpiolib.c`, registration initializes the valid mask before calling `get_direction()` for each valid GPIO; the Qualcomm callback reads its control register. The previous unmasked node could thus read these reserved GPIOs even without consumers. This is a plausible cause of the reboot, not a confirmed diagnosis. The corrected overlay remains untested on hardware, and the TLMM module stays excluded pending a controlled diagnostic plan.

Validation: the all-core build (`--jobs 32`) completed, the compiled overlay's reserved GPIO set matches the MiCode source exactly, and the generated initramfs contains the failure diagnostic but no `pinctrl-sm8750.ko`. Shell syntax, shellcheck for the build entry point, and whitespace checks passed. Debian commit `c84d456` produces the corrected DTBO with SHA256 `24f0695b18fde2a01dab977903c0243b57b0d67cc06173dadc6dedacf9ad4459`; the matching boot image SHA256 is `fe1c2388e47944b5a24255526a85452e758e8c30b53841b9b55c36e418a76756`. Artifacts are in `debian-piano/out/adsp-m1/`; neither was deployed during this source review. All three repositories remain descendants of their `milestone-1` tags.

The old working DTBO SHA256 `26fe3551ddf82187e11d3c56ad5a936e980ef6889e37f5741d5610b422078a7a` is retained in `debian-piano/out/adsp-m1/rollback-spi-good.dtbo.img`.
