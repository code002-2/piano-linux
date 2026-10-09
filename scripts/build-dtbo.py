#!/usr/bin/env python3
"""build-dtbo.py — compile the piano bring-up DTBO image.

Usage:
    scripts/build-dtbo.py [--dts PATH] [--output PATH] [--dtc dtc]

Compiles debian-piano/boot/dtbo-piano-bringup.dts with `dtc -@` (kernel
dt-bindings include path resolved from the sibling linux-piano checkout)
and wraps the result in a single-entry big-endian Android DTBO table
header:

    struct dt_table_header (all fields big-endian):
        magic          0xd7b7ab1e
        total_size     header + entry header + dtb
        header_size    32
        dt_entry_size  32
        dt_entry_count 1
        dt_entries_off 32
        page_size      4096
        version        0
    struct dt_table_entry (big-endian):
        dt_size, dt_offset(=64), id, rev, custom[4]

The bring-up overlay is the stock piano dtbo entry 0 (all downstream
fragments intact) plus:

  * fragment@200 — simple-framebuffer node at the cont_splash memory
    (0xfc800000, 0x2b00000): dual-DSI full width 3200 x 2136,
    stride 12800, a8b8g8r8.  Quad-split console output is the symptom of
    declaring 1600/6400 against the real 3200-wide scanout.
  * /reserved-memory/splash_region gains `no-map` so simpledrm's
    devm_ioremap_wc does not collide with the linear mapping.
  * fragments@201..214 — mainline DT provider overrides + the dwc3 USB
    stack (gcc/tcsr/icc/rpmh-clk/smmu providers, rpmhpd, bi_tcxo_div2,
    M31 eUSB2 + QMP combo phys, the usb@a600000 wrapper in mainline
    binding, downstream nested dwc3 child disabled).  Phandle references
    inside these fragments resolve against the stock tree labels via
    the overlay __symbols__ mechanism.

ABL phandle-rewrites the merged tree, so added fragments must only use
stock-tree labels (via `target = <&label>`) or target-path — never
hard-coded phandle numbers.

Flashed to dtbo_b only (`fastboot flash dtbo_b`).  Never write
bootloader-chain partitions.
"""

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

MAGIC = 0xD7B7AB1E


def build(repo: Path, dts: Path, output: Path, dtc: str) -> None:
    kinc = repo.parent / "kernel" / "include"
    with tempfile.TemporaryDirectory() as td:
        dtb = Path(td) / "overlay.dtb"
        pp = Path(td) / "overlay.pp.dts"
        subprocess.run(
            ["cpp", "-nostdinc", "-undef", "-x", "assembler-with-cpp",
             "-I", str(kinc), "-o", str(pp), str(dts)],
            check=True,
        )
        subprocess.run(
            [dtc, "-@", "-I", "dts", "-O", "dtb", "-o", str(dtb), str(pp)],
            check=True,
        )
        blob = dtb.read_bytes()
    entry = struct.pack(">8I", len(blob), 64, 0, 0, 0, 0, 0, 0)
    total = 32 + 32 + len(blob)
    header = struct.pack(">8I", MAGIC, total, 32, 32, 1, 32, 4096, 0)
    output.write_bytes(header + entry + blob)
    print(f"build-dtbo: wrote {output} ({total} bytes, dtb {len(blob)})")


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--dts", type=Path,
                    default=repo / "boot" / "dtbo-piano-bringup.dts")
    ap.add_argument("--output", type=Path,
                    default=repo / "out" / "test-image" / "dtbo-piano-bringup.img")
    ap.add_argument("--dtc", default="dtc")
    args = ap.parse_args()
    if not args.dts.is_file():
        print(f"build-dtbo: missing overlay source {args.dts}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    build(repo, args.dts, args.output, args.dtc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
