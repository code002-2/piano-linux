#!/usr/bin/env python3
"""Generate a minimal AVB vbmeta image with verification disabled.

The header layout is fixed by libavb (external/avb/libavb/avb_vbmeta_image.h,
struct AvbVBMetaImageHeader): 256 bytes, all fields big-endian, no descriptors,
algorithm NONE, no auth/aux blocks. flags=3 means disable-verity (1) plus
disable-verification (2) -- the standard unlocked-bootloader flashing step.

Output is written to the path given as the only argument.
"""

import struct
import sys

AVB_MAGIC = b"AVB0"
HEADER_FORMAT = (
    ">4s"          # magic
    "II"           # required_libavb_version_major/minor
    "QQ"           # authentication_data_block_size, auxiliary_data_block_size
    "I"            # algorithm_type (0 = NONE)
    "QQQQ"         # hash_offset, hash_size, signature_offset, signature_size
    "QQ"           # public_key_offset, public_key_size
    "QQ"           # public_key_metadata_offset, public_key_metadata_size
    "QQ"           # descriptors_offset, descriptors_size
    "Q"            # rollback_index
    "I"            # flags (3 = disable-verity | disable-verification)
    "I"            # rollback_index_location
    "48s"          # release_string
    "80s"          # reserved
)
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 256, HEADER_SIZE

RELEASE = b"piano-disable-vbmeta".ljust(48, b"\0")
    "QQ"           # public_key_metadata_offset, public_key_metadata_size
    "QQ"           # descriptors_offset, descriptors_size
    "Q"            # rollback_index
    "I"            # flags (3 = disable-verity | disable-verification)
    "I"            # rollback_index_location
    "48s"          # release_string
    "80s"          # reserved
)
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 256, HEADER_SIZE

RELEASE = b"piano-disable-vbmeta".ljust(47, b"\0")[:47] + b"\0"


def build() -> bytes:
    header = struct.pack(
        HEADER_FORMAT,
        AVB_MAGIC,
        1, 0,                     # libavb version 1.0
        0, 0,                     # no auth/aux blocks
        0,                        # algorithm NONE -> no hash/signature
        0, 0, 0, 0,
        0, 0,
        0, 0,
        0,                        # descriptors right after header, size 0
        0,                        # rollback_index
        3,                        # flags: disable-verity | disable-verification
        0,                        # rollback_index_location
        RELEASE,
        b"\0" * 80,
    )
    assert len(header) == 256
    return header  # no descriptors, no padding: flash target is a raw partition


def parse_verify(image: bytes) -> None:
    """Independent re-parse (different code path) to catch layout mistakes."""
    magic = image[0:4]
    assert magic == AVB_MAGIC, magic
    (lib_major, lib_minor, auth_sz, aux_sz, algo) = struct.unpack_from(">IIQQI", image, 4)
    flags = struct.unpack_from(">I", image, 120)[0]
    total = 256 + auth_sz + aux_sz
    assert (lib_major, lib_minor) == (1, 0)
    assert (auth_sz, aux_sz, algo) == (0, 0, 0), "must be unsigned, descriptor-less"
    assert flags == 3, flags
    assert len(image) == total, (len(image), total)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} OUTPUT")
    image = build()
    parse_verify(image)
    with open(sys.argv[1], "wb") as f:
        f.write(image)
    print(f"wrote {sys.argv[1]} ({len(image)} bytes, flags=3, algorithm=NONE)")


if __name__ == "__main__":
    main()
