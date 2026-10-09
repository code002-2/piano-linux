#!/usr/bin/env python3
"""Convert a raw ext4 image to an Android sparse image for fastboot.

Only allocated blocks are stored (RAW chunks of at most 64 MiB); the ext4
free-block map becomes DONT_CARE, like the stock userdata.img.  img2simg's
default output instead turns every zero block into FILL chunks, and piano's
ABL physically writes FILL data: for a mostly-empty 12 GiB image that is
~10 GiB of slow zero writes and looks like a fastboot hang.  img2simg -s is
no substitute: mke2fs zeroes inode tables and the journal by punching holes,
so hole-as-DONT_CARE would leave stale Android bytes in ext4 metadata.
"""
import os
import re
import struct
import subprocess
import sys

MAX_CHUNK = 64 << 20


def free_ranges(image):
    out = subprocess.run(['dumpe2fs', image], check=True, capture_output=True,
                         text=True, env={**os.environ, 'LC_ALL': 'C'}).stdout
    block_size = int(re.search(r'^Block size:\s+(\d+)', out, re.M).group(1))
    block_count = int(re.search(r'^Block count:\s+(\d+)', out, re.M).group(1))
    ranges = []
    for line in re.findall(r'^\s+Free blocks: (.*)$', out, re.M):
        for part in filter(None, (p.strip() for p in line.split(','))):
            a, _, b = part.partition('-')
            ranges.append((int(a), int(b or a) + 1))
    return block_size, block_count, sorted(ranges)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: ext4-to-simg.py <raw ext4 image> <sparse output>')
    raw, sparse = sys.argv[1:]
    bs, total, free = free_ranges(raw)
    # (type, start block, length) covering [0, total) in order.
    extents, pos = [], 0
    for a, b in free:
        if a > pos:
            extents.append(('raw', pos, a - pos))
        extents.append(('dontcare', a, b - a))
        pos = b
    if pos < total:
        extents.append(('raw', pos, total - pos))
    per_chunk = MAX_CHUNK // bs
    chunks = []
    for kind, start, length in extents:
        if kind == 'dontcare':
            chunks.append((kind, start, length))
            continue
        for off in range(0, length, per_chunk):
            chunks.append((kind, start + off, min(per_chunk, length - off)))
    with open(raw, 'rb') as src, open(sparse, 'wb') as dst:
        dst.write(struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, bs,
                              total, len(chunks), 0))
        for kind, start, length in chunks:
            if kind == 'dontcare':
                dst.write(struct.pack('<HHII', 0xcac3, 0, length, 12))
                continue
            dst.write(struct.pack('<HHII', 0xcac1, 0, length, 12 + length * bs))
            src.seek(start * bs)
            data = src.read(length * bs)
            if len(data) != length * bs:
                sys.exit('ext4-to-simg: raw image shorter than its filesystem')
            dst.write(data)
    stored = sum(n for k, _, n in chunks if k == 'raw') * bs
    print(f'ext4-to-simg: {len(chunks)} chunks, {stored >> 20} MiB stored '
          f'of {total * bs >> 20} MiB')


if __name__ == '__main__':
    main()
