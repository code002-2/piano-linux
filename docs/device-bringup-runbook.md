# Xiaomi Pad 8 Pro (piano) — first-light device runbook

Scope: the RAM-boot (`fastboot boot`) test session using the `piano/test-bringup` kernel and the `debian-piano` test image (`out/test-image/`). Everything here is **read-only for the device's permanent storage**: no partition is written, erased or flashed at any point. Rebooting returns the device to stock Android on its current slot.

## 1. Safety audit (2026-09-19, offline review)

Findings from reviewing every branch/artifact that touches the device:

| # | Area | Finding | Risk | Mitigation |
|---|---|---|---|---|
| 1 | Boot flow | Test images boot via `fastboot boot` only; no flash/write path exists anywhere in `debian-piano` or the kernel branches (no `dd`, `mkfs`, `flash` commands in any shipped script; initramfs `/init` explicitly never writes to partitions and never `switch_root`s) | none (RAM only) | keep it that way; flashing needs a separate, explicit decision |
| 2 | Bootloader chain | Untouched: images contain only kernel+ramdisk+dtb; `abl/xbl/tz/hyp/...` never referenced | none | hard rule (AGENTS.md §5) |
| 3 | dtbo interaction | Stock `dtbo_b` stays in place; abl may try to apply stock overlays onto our DTB — symbols (`qupv3_se2_spi`, …) don't exist upstream, so an overlay apply may fail | boot aborts **before the kernel runs** (costless); OQ#7 territory | variant ladder below; never flash dtbo during RAM-boot testing |
| 4 | Display panel power | `display_panel_vsp/vsn` fixed regulators (GPIO 117/118, ~5.8 V) are `always-on` (OQ#17); stock also holds them at boot handoff | panel stress/heat on long sessions | keep sessions short; test script blanks the screen and drops backlight when done; power off between sessions |
| 5 | Panel rail mapping | `vci`/`vdd` assignments carried from the SM8750 MTP (OQ#16) — piano's downstream supply table lists only vddio+VSP/VSN | wrong-rail voltage if wiring differs — **main hardware risk of the display test** | first boot: watch dmesg + panel; if the panel stays dark, stop and collect evidence, do not iterate blindly |
| 6 | Touch firmware | DT pins the CSOT-family blob; the panel family is unknown until read (OQ#15). A wrong-family blob fails CRC inside the no-flash touch IC | touch init failure (recovers on reboot) | both families ship in the image; if CSOT fails, rebuild with the BOE `firmware-name` |
| 7 | Battery/charging | Mainline pmic-glink charger stack is unvalidated; the device runs on battery during the test | battery drain (fuel-gauge protection shuts down safely) | start > 50 %, keep USB connected, abort at < 20 % |
| 8 | GPU | The experimental GPU branch (`bp/gpu-v1`) is **not** part of this image | none in this test | GPU validation is a separate, later session |
| 9 | initramfs network | USB-NCM point-to-point only (usb0, 10.42.0.0/24); no other interface comes up | exposure limited to the plugged-in host | fixed test MACs; udhcpd pool on the link only |
| 10 | initramfs SSH | dropbear: pubkey (per-build ed25519) and/or root password (SHA-512 hash) or blank password (`-B`); no other services listen | someone with the USB cable can log in — accepted for bring-up | auth material lives only in gitignored `out/`; rebuild to change; remove blank/password modes once rootfs matures |
| 11 | Boot-image params | All from stock-ROM CONFIRMED values (`boot/stock-boot-params.env`); cmdline is console-only (operator policy) | none | provenance gate refuses UNVERIFIED values for deliverables |

## 2. Preconditions

- Unlocked bootloader (user's own action), battery > 50 %.
- Slot A stock Android bootable; rescue fastboot ROM in `local/rom/`.
- Host with `fastboot` (android-tools), SSH client, and the workspace.
  - Verified on the bring-up host (2026-09-21): `fastboot` runs without sudo from a local session — systemd's `70-uaccess.rules` tags ADB/ fastboot USB devices (interface classes ff4201/ff4203) `uaccess`, and logind grants the seated user ACL access to the device node. OpenSSH 10.5p1 client is ready.
- Built image set: `debian-piano/out/test-image/` (MANIFEST.txt + 5 images).

## 3. Boot order (each attempt is costless)

> **SUPERSEDED 2026-09-22 by §7**: the v2 primary below is a documented dead end on this device (silent ABL rejection). Use the v4 RAM-boot contract in §7; the ladder below is kept for the record.

```
fastboot devices                      # device visible
fastboot getvar current-slot          # RECORD it
fastboot getvar unlocked              # expect: yes

# primary — single image
fastboot boot piano-test-boot-v2.img

# optional abl-acceptance probe — v4 boot with EMPTY ramdisk, no dtb.
# Only tells whether abl accepts a RAM boot at all: the kernel starting
# and then stopping is EXPECTED (no initramfs/dtb inside this image).
fastboot boot piano-test-boot.img
```

Why no dual-image commands: the host fastboot (Debian android-tools 37.0.0, AOSP `FB_CMD_BOOT`: `boot KERNEL [RAMDISK [SECOND]]`) accepts exactly ONE image per `boot`; passing a second boot.img dies with `cannot boot a boot.img *and* ramdisk` (string confirmed in the host binary). So the v4 combos (`boot`+`vendor_boot`, `boot-ramdisk`+`vendor_boot-dtb`) have **no RAM path on this host**. They become available again if the host fastboot's own usage text shows multi-image boot support.

**If the primary (v2) is rejected by abl** (error, device returns to fastboot): record the raw `fastboot` output and STOP — report it, do not improvise. Rationale: the v4 dual-image combos are unusable here (see above); flashing `vendor_boot_b` is NOT permitted (AGENTS.md writable whitelist is `boot_b`/`dtbo_b`/userdata-derived only), so it is not a bypass; and any new workaround (e.g. an Image+dtb concatenated sixth variant) is new content that needs explicit user approval first.

## 4. After boot

Expected: kernel console text **on the panel** (fbcon — the display test) and/or a USB network interface on the host (the gadget runs udhcpd; the host may also use static 10.42.0.1/24).

```
ssh -i debian-piano/out/test-image/piano-test-ssh-ed25519 root@10.42.0.2
# or, built with --root-password:  ssh root@10.42.0.2   (then password)
# or, built with --root-password '': press enter at the password prompt

piano-tests          # interactive menu below (9 tests + status matrix)
```

- **Touch** (`1`): enables `/proc/nvt_thp_raw` and streams decoded THP frames (sequence, validity flags, CRC, first event bytes) for 30 s — touch the screen. Pass: `VALID` frames with increasing sequence.
- **Display** (`2`): DRM connector status, fb geometry, full-screen R/G/B/W/K fields + noise via `/dev/fb0`, held 2 s each. Pass: fields visible on the panel.
- **Audio** (`3`): walks the ADSP → soundwire → wcd9395 → wsa884x soundcard chain; `3a` additionally plays a 3 s 440/880 Hz tone through the speakers (manual listen: audible = full analog chain works).
- **Battery** (`4`): pmic-glink/battmgr telemetry (capacity, status, charge types) — requires ADSP running.
- **WLAN** (`5`): pcie0 enumeration → ath12k probe, optional scan.
- **Bluetooth** (`6`): uart14 serdev + pwrseq + hci0 bring-up.
- **Collect** (`7`, verbose `7v`): one-shot evidence tarball (dmesg, /proc state, DRM/touch status); `scp root@10.42.0.2:/run/piano-evidence-*.tar.gz .`
- **dmesg** (`8`): subsystem-filtered tail (panel/DRM/touch/USB/PMIC/ remoteproc/audio/ath12k/qca).
- **probe** (`9`): refresh the status matrix.
- A boot smoke report is written automatically to `/run/boot-smoke.log` by `piano-tests --auto` ~5 s after boot (status matrix + evidence tarball, non-interactive).

### 4b. Device coverage & first-boot expectations

Kernel `7.2.6-00013-g2f4a243cb610` (`piano/test-bringup`). What the image is prepared to bring up on first boot, and where it may not:

| Subsystem | DT node | Driver / module | Firmware in image | First-boot expectation | Known risk |
|---|---|---|---|---|---|
| Display | mdss_mdp + dsi0 + NT37801 panel | `DRM_MSM=y`, `DRM_PANEL_NOVATEK_NT37801=y` (fbcon) | — | kernel console text on the panel | panel family unknown (OQ#15); vci/vdd rail mapping from MTP (OQ#16); VSP/VSN regulators always-on → heat (OQ#17) |
| Touch | spi2 + nt36532e | `nt36532e_ts=m` + `spi-geni-qcom=m` | `novatek/novatek_nt36532_piano_fw_csot.bin` (CSOT pinned in DT; BOE variant also shipped) | probes after the panel; `/proc/nvt_thp_status` exists | wrong-family blob → CRC fail inside the IC, recovers on reboot; avdd/lcd-id GPIOs unmanaged (OQ#18) |
| USB-NCM | dwc3 gadget usb0 | built-in (`=y`: libcomposite + NCM, M31 eUSB2 + QMP combo phys); `pmic_glink_altmode=m` loaded by /init for Type-C role/orientation (gadget defaults to peripheral without it, USB2 speed only) | — | host gets 10.42.0.2/24, ssh works | — |
| Battery | pmic_glink → battmgr | `pmic_glink=m`, `qcom_battmgr=m` | via ADSP image | capacity/status readable once ADSP runs | needs the shipped `piano-pd-locator` for the glink domain |
| ADSP/CDSP | remoteproc `adsp`/`cdsp` | `qcom_q6v5_pas=m` | `qcom/sm8750/{adsp,cdsp}.mbn` (84 files: mdt→mbn renamed + bNN segments) | remoteproc state `running` for both | first load of a vendor firmware on mainline — watch dmesg |
| Audio | sm8750 sndcard + wcd9395 + wsa884x + soundwire | sc8280xp/wcd939x/wsa884x/soundwire chain `=m` | via ADSP image | `/proc/asound/cards` lists the card | WSA884x vs 883x amp variant decided by SDW enumeration (OQ#21) |
| WLAN | pcie0, PCI 17cb:110e | `pcie-qcom=y` + `qmp-pcie phy=m` (packed, loaded by /init), `ath12k=m` (ID added, probes the WCN7850 path) | `ath12k/WCN7850/hw2.0/` (4 files) + board data | PCI enum → MHI → QMI → wiphy | CE config / firmware family may differ from WCN7850 hw2.0 (OQ#20) |
| Bluetooth | uart14 serdev + pwrseq | `hci_uart=m`, `btqca`, `pwrseq-qcom-wcn=m` | `qca/` hmt family (6 files) | `hci0` appears with an address | same combo rails/clock as WLAN (OQ#20) |
| GPU | absent | not in this image | — | — | GPU validation is a separate later session (`bp/gpu-v1` is not merged into `piano/test-bringup`) |

## 5. Abort criteria (stop, hold power, collect evidence)

- Panel area or SoC area becomes noticeably hot, or any burning smell.
- Panel stays dark AND the VSP/VSN area heats (rail mapping suspect — OQ#16).
- Battery below 20 %.
- Primary (v2) rejected by abl — record the raw `fastboot` output and stop (§3). Repeated silent aborts of the primary likewise.

## 6. Rebuild / customize

One command from the workspace root (ties linux-piano + debian-piano + local/ together; refuses to build unless linux-piano is on `piano/test-bringup` and debian-piano on `main`, both clean):

```
debian-piano/scripts/fetch-arm64-tools.sh --output-dir out/arm64-tools   # once
scripts/build-test-image.sh --jobs "$(nproc)"
```

It configures the kernel from `piano_defconfig` in `linux-piano/out/test-image` (boot-critical and touch options are gated), builds Image + modules, packs the debug initramfs (NCM/telnetd environment, touch module closure, touch firmware from `local/firmware/odm/firmware/`, test scripts, `piano-touch-view`), embeds it and writes exactly `boot.img`, `dtbo.img` (overlay `boot/dtbo-piano-touch-v2.dts`) and `MANIFEST.txt` into `debian-piano/out/test-image/`. Touch bring-up details: `docs/touch-bringup-v2.md`.

## 7. Measured boot contract (2026-09-22, on-device)

Everything below was verified against the real device on 2026-09-22 and supersedes the §3 ladder and the §4b expectations table.

### 7.1 What ABL actually accepts

| Variant | Result |
|---|---|
| v0 boot.img (gzip/raw, ±appended DTB, stock DTB, sheng tags recipe) | **silent rejection** — `OKAY`, gadget stays, no USB bounce. Any DTB in a v0 kernel region is refused, regardless of content |
| custom vendor_boot (our DTB / marker ramdisk) | **silent rejection** |
| **v4 `fastboot boot` + CURRENT SLOT's stock vendor_boot + current-slot dtbo** | **kernel executes** (USB drop at ~7-10 s, no fall-back, panel backlight on) |

Consequences: the device tree the kernel sees is the **stock vendor DTB + dtbo_b**, never our own DTB; every mainline node must be injected as a DTBO *fragment* (rules in §7.3). The stock cmdline (`console=ttynull`) is countered by `CONFIG_CMDLINE_FORCE`. Current slot must be `b` (`fastboot set_active b`) because RAM boot composes slot-b images.

### 7.2 Panel console (simpledrm)

`dtbo_b = dtbo-piano-bringup.img` (source: `debian-piano/boot/dtbo-piano-bringup.dts`, built byte-identical by `debian-piano/scripts/build-dtbo.py`).

- All stock fragments kept intact; one added `fragment@200` (`target-path="/"`, zero phandle deps): `framebuffer@fc800000` over the cont_splash memory (0xfc800000 / 0x2b00000).
- Geometry: the panel is **dual-DSI**: full width **3200** (1600 per DSI side) x 2136, stride **12800**, `a8b8g8r8`. Declaring 1600/6400 renders as four quadrants: top two duplicated console lines, bottom two black (real scanout consumes two 6400-byte console rows per display row).
- `/reserved-memory/splash_region` needs **`no-map`**: without it simpledrm's `devm_ioremap_wc` hits the linear mapping → panic at probe.
- ABL rewrites phandles in the merged tree — hard phandle references from added fragments always dangle. `target-path="/"` only.

Kernel side (all `=y`): `DRM_SIMPLEDRM` (binds the DT `simple-framebuffer` node; replaces `FB_SIMPLE`), `DRM_PANIC` + `DRM_PANIC_SCREEN="qr_code"` + `DRM_PANIC_SCREEN_QR_CODE` (needs `RUST`; bindgen from `cargo install bindgen-cli`, rust-src from the `rust-src` package), `FONT_TER16x32`, `CONFIG_CMDLINE_FORCE` with `console=tty0 loglevel=8 fbcon=font:TER16x32 rdinit=/beaconinit`. Kernel panics render as a big QR code encoding the kmsg tail — scan it with a phone, no OCR needed.

### 7.3 The init override (root cause of the first "panic")

During v4 RAM boot ABL concatenates ramdisks from the CURRENT SLOT's init_boot (first-stage init — verified: the stock vendor_ramdisk itself carries only 448 flat dlkm modules + `first_stage_ramdisk/fstab.qcom`, no `/init`) and vendor_boot (dlkm) **after** our ramdisk; cpio cascade rules make later archives win, so Android first-stage init replaced our `/init` (it died mounting selinuxfs → `Attempted to kill init`, which looked like a boot panic). Fix: the same script ships as `/beaconinit` and `rdinit=/beaconinit` (forced cmdline) selects it. Any future ramdisk entry point MUST NOT be called `/init`. Our `/lib/modules/<kver>/` tree does not collide with the flat `/lib/modules/*.ko` dlkm layout.

### 7.4 Still open (next sessions)

- USB/dwc3: mainline `sm8750.dtsi` has `usb@a600000` + m31-eusb2 + qmp-usb3-dp PHYs, but binding needs gcc/tcsrcc/pdc/rpmhpd/smmu/ interconnect providers under mainline naming — a staged DTBO effort (override providers first), not a one-shot fragment. Gadget/NCM side is already `=y`.
- Log read-back without display/USB: `oem lkmsg` (95 kB kernel log via DATA phase; needs `out/fastboot-data-cmd.py`, the stock fastboot CLI discards the payload). Registering our own dump region in the IMEM dump table is explored but unverified.
- §4b's device table remains aspirational until the DTB-side providers land; drivers `=y`/`=m` states in it are still accurate as built.

### 7.5 ABL partition-state poisoning (2026-09-22 evening incident, recovered 2026-09-23)

Symptom: `fastboot boot` downloads OKAY, but "Booting" drops USB instantly, the device resets, ABL flags slot b unbootable (`getvar slot-unbootable:b` → yes) and falls back to Android (slot a). The kernel never executes — `oem lkmsg` via `out/fastboot-data-cmd.py` shows zero mainline lines, only the last Android kernel log. This signature means **ABL aborted before the jump**, not a kernel crash.

Cause: the current-slot partition set had been left inconsistent (custom `boot_b` / `vbmeta_b` experiments written on top of the otherwise stock set during an evening session). The v4 RAM-boot path still composes and verifies current-slot metadata alongside the downloaded image; a mismatched pair aborts the boot. Byte-identical images and repo-level reverts cannot fix this — it is device persistent state, not code. One whole debugging night was spent rebuilding perfect images against a poisoned device.

Recovery (verified working 2026-09-23, telnet SHELL-OK re-confirmed):
1. Stock **no-wipe** fastboot ROM reflash — restores every slot partition to a self-consistent stock set; userdata survives.
2. `fastboot set_active b`
3. `fastboot flash dtbo_b dtbo-usb-nopd9.img` — the single non-stock piece the working combo needs (stock dtbo walks into the M31 eUSB2 init hang).
4. `fastboot boot piano-test-boot-usb31.img` → USB drop ~7–10 s, NCM NIC enumerates on the host, `telnet 10.42.0.2 23` answers.

Rule: never leave custom `boot_b`/`vbmeta_b` content sitting on the device alongside stock counterparts. If "Booting" fails instantly and lkmsg has no mainline output, suspect partition state before touching any code.

## 8. USB NCM debug network (2026-09-22, achieved)

The dwc3 UDC is up and carries a usable debug network — this closes the "dwc3 UDC → USB NCM gadget" sub-goal.

### 8.1 Working combination

- Kernel: `linux-piano` branch `piano/usb-udc-bringup` (PR #8): empty- extcon handling in `dwc3_get_extcon()`, icc degrade in `dwc3-qcom`, corrected `qcom,msm-id`, `CONFIG_SM_TCSRCC_8750=y`.
- DTBO: `debian-piano/boot/dtbo-piano-usb-nopd9.dts` → `dtbo_b`.
- Initramfs: `debian-piano` branch `piano/usb-ncm-initramfs` (PR #11): `beaconinit` NCM gadget + udhcpd + telnetd.
- Build: `scripts/build-test-image.sh` from the workspace root — builds the initramfs, embeds it via `CONFIG_INITRAMFS_SOURCE` (out/.config is preserved, never regenerated — see §7.5 and the script's option gate), builds `Image`, and packs exactly three deliverables: `debian-piano/out/test-image/{boot.img,dtbo.img,MANIFEST.txt}`. Then follow the MANIFEST recipe: `fastboot set_active b` → `fastboot flash dtbo_b dtbo.img` → `fastboot boot boot.img`.

### 8.2 Host side

```
nmcli connection add type ethernet ifname <usb-nic> con-name piano-ncm \
    ipv4.method manual ipv4.addresses 10.42.0.1/24 ipv6.method disabled
nmcli connection up piano-ncm
nc 10.42.0.2 23        # busybox telnetd remote shell (no auth)
```

Device side: `usb0` at 10.42.0.2/24, telnetd on :23, dropbear attempted on :22 (dynamically linked in the current initramfs — falls back).

### 8.3 Known gaps (tracked separately)

- **M31 eUSB2 init sequence hangs the SoC bus** (silent async death, no oops). The USB2 consumer points at `usb_nop_phy` (legacy `usb-nop-xceiv`), so dwc3 runs on the bootloader-configured PHY state. Re-enabling `m31eusb2_phy_init()` requires first making the phy's register writes safe (BCR reset ordering / register-clock gating).
- **RPMh RSC probe fails `-EINVAL`** on both `adc8000.rsc` and `af20000.rsc`; that keeps the interconnect providers out of `sync_state` and is why `dwc3-qcom` degrades past the usb-ddr icc path instead of blocking on it.
- **Historical dropbear failure:** the staged dynamic closure lacked `libgmp.so.10` (required by libtomcrypt). The GNOME/rootfs work adds `libgmp10` to the tools fetcher; refresh old `out/arm64-tools` trees. Authenticated arm64 dropbear SSH was exercised under QEMU user emulation.

### 8.4 Debugging scars worth remembering

- `piano_fb_stamp()` framebuffer staging proved the crash point survived a hard hang (printk never flushes); full-screen background colour per step beat counting pixel squares. Removed again once the root cause (M31 init) was pinned down.
- A shell syntax error in `beaconinit` (empty `if` body) kills pid 1 and panics the kernel with "Attempted to kill init" — always `bash -n` the init script before shipping it.
- `build-initramfs.sh` must be invoked with full arguments (`--busybox/--dropbear-tree --output`); invoking it bare prints usage and exits while looking deceptively like a successful build.

## 9. Debian GNOME on userdata (milestone-boot)

From the workspace root, `scripts/build-rootfs-image.sh` builds a matched boot/dtbo/raw-ext4/sparse-userdata set. Full prerequisites, credentials, destructive userdata warnings and the operator flashing/acceptance procedure are in `debian-piano/README.md`. No script flashes the device.

The kernel's `piano_rootfs.config` is a separate profile over `piano_defconfig`: `/pianoinit`, forced userdata root, simpledrm, no PAS/native MSM display. The legacy beacon test profile remains available. SMMU setup precedes switch_root; failures stop in key-only USB SSH rescue. GNOME uses software rendering, scale 2, no idle blanking/suspend. WLAN, Bluetooth and THP input start through ordered services instead of test loops.

Stock ABL appends Android's `/init`, so a unique `/pianoinit` entry point is required. The rootfs-only overlay adds UFS host/PHY mainline bindings, clears the stock IOMMU dependency, and preserves ABL's storage rails. Its PHY module loads only after SID 0x60 is matched and the UFS CLKREF bit at 0x0f205008 is enabled.

Two device lessons from the first boot (milestone-boot):

- **Sparse userdata must use DONT_CARE for free space.** `img2simg` encodes zero blocks as FILL chunks and ABL writes them physically: a 12 GiB image became ~10 GiB of zero writes, which looks like a fastboot hang (an interrupted flash then leaves a corrupt root). `ext4-to-simg.py` stores only allocated blocks, like the stock userdata.img.
- **No udev autoload on SoC buses.** The stock DT carries many nodes with unvalidated mainline drivers; coldplug autoloading froze the display right after `systemd-udev-trigger`. SoC drivers are loaded only by the ordered piano units.

The Debian workflow builds boot/dtbo/userdata from one build with the firmware from the separate piano-firmware repository (see its compliance statement), and uses persistent ccache. Its cross-repo refs must include both companion changes before default CI builds can succeed.

Writing the whole userdata partition destroys Android data; leaving slot A's boot partitions untouched does not preserve that data or prevent Android from overwriting Debian on a subsequent Android boot. Never advertise on-device GNOME, WiFi association, BT pairing, battery or charging acceptance based only on a successful host build.
