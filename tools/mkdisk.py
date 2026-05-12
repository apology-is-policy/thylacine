#!/usr/bin/env python3
# tools/mkdisk.py — deterministic disk-image generator (P4-Ic7).
#
# Produces a raw block image with:
#   - sector 0  : bytes [0..16) = "THYLACINE-DISK-1" + bytes [16..512) = 0
#                 (preserves the signature the P4-Ic5b2 virtio-blk-probe checks).
#   - sector k>0: bytes [0..512) = pattern_a(k), the per-sector LCG-A stream.
#
# Pattern A (per-sector, byte-identical between this generator and the
# verifier in usr/virtio-blk-rw/src/main.rs):
#
#     state = k                          # u64
#     for i in 0..64:                    # 64 * 8 bytes = 512
#         state = (state * MUL_A + INC_A) mod 2**64
#         out[i*8 .. (i+1)*8] = state.to_bytes(8, "little")
#
# Constants are pinned (drift means the userspace verifier sees a
# byte-level mismatch and the test reports FAIL).
#
# Usage:
#   tools/mkdisk.py <out.img> <size-bytes>
#
# Example:
#   tools/mkdisk.py build/disk.img 16777216
#
# Cost: writes <size-bytes> in chunks of CHUNK_SECTORS (= 8192 sectors
# = 4 MiB) per loop iteration. ~16 MiB completes in <500 ms on M-series
# hardware; 1 GiB completes in ~30 s.

import struct
import sys

SECTOR_SIZE = 512
SIGNATURE = b"THYLACINE-DISK-1"     # 16 bytes; matches usr/virtio-blk-probe + usr/virtio-blk-rw

# Pattern A LCG constants (mirrored verbatim in usr/virtio-blk-rw/src/main.rs::PATTERN_A_MUL/INC).
MUL_A = 0x9E3779B97F4A7C15
INC_A = 0x1234567890ABCDEF
MOD64 = 1 << 64

# Per-iteration sector batch. Larger batches amortize Python overhead;
# 4 MiB keeps peak RSS bounded.
CHUNK_SECTORS = 8192

def sector_pattern_a(k: int) -> bytes:
    state = k & 0xFFFFFFFFFFFFFFFF
    buf = bytearray(SECTOR_SIZE)
    for i in range(64):
        state = (state * MUL_A + INC_A) % MOD64
        struct.pack_into("<Q", buf, i * 8, state)
    return bytes(buf)

def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <out.img> <size-bytes>", file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    try:
        size = int(sys.argv[2])
    except ValueError:
        print(f"size-bytes must be an integer: {sys.argv[2]}", file=sys.stderr)
        return 2
    if size < SECTOR_SIZE:
        print(f"size-bytes ({size}) below SECTOR_SIZE ({SECTOR_SIZE})", file=sys.stderr)
        return 2
    if size % SECTOR_SIZE != 0:
        print(f"size-bytes ({size}) not a multiple of SECTOR_SIZE ({SECTOR_SIZE})", file=sys.stderr)
        return 2

    total_sectors = size // SECTOR_SIZE

    sector_0 = bytearray(SECTOR_SIZE)
    sector_0[0:16] = SIGNATURE

    with open(out_path, "wb") as f:
        f.write(sector_0)
        k = 1
        while k < total_sectors:
            batch_end = min(k + CHUNK_SECTORS, total_sectors)
            chunk = bytearray((batch_end - k) * SECTOR_SIZE)
            for j, sk in enumerate(range(k, batch_end)):
                chunk[j * SECTOR_SIZE : (j + 1) * SECTOR_SIZE] = sector_pattern_a(sk)
            f.write(chunk)
            k = batch_end
    return 0

if __name__ == "__main__":
    sys.exit(main())
