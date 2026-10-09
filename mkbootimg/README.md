# Vendored AOSP mkbootimg tools

The Python tools in this directory are vendored verbatim from AOSP:

- Repository: https://android.googlesource.com/platform/system/tools/mkbootimg
- Branch: `main`
- Source commit: `d2bb0af5ba6d3198a3e99529c97eda1be0b5a093`

Files:

| File | Upstream path |
|---|---|
| `mkbootimg.py` | `mkbootimg.py` |
| `unpack_bootimg.py` | `unpack_bootimg.py` |
| `repack_bootimg.py` | `repack_bootimg.py` |
| `gki/generate_gki_certificate.py` | `gki/generate_gki_certificate.py` (runtime dependency of `repack_bootimg.py`) |

All files retain their original Apache License 2.0 headers and copyright
notices (The Android Open Source Project). No modifications have been made;
if a local patch ever becomes necessary it must be called out here and kept
minimal.

Not copied from upstream: `Android.bp`, `BUILD.bazel`, `rust/`, `tests/`,
`include/` and other Android build metadata — irrelevant for the standalone
Python usage here.

Upstream usage reference:
`mkbootimg.py --help`, `unpack_bootimg.py --help` document the full
interfaces.
