#!/usr/bin/env python3
"""Build a standalone sparse image for the tail of an Android sparse image.

`fastboot flash userdata big.simg` splits the image into <= max-download-size
pieces with libsparse's sparse_file_resparse().  On fleur the device rejects
the very last piece with "sparse image write failure"; flashing that same tail
alone (with a DONT_CARE prefix for the leading blocks) works.

This script reconstructs that tail.  It simulates libsparse's resparse
algorithm (import -> merge adjacent FILL blocks -> move_chunks_up_to_len(),
including the 1/8 split heuristic) to find the block where the final segment
starts, then writes:

    sparse header (total_blks = whole image)
    DONT_CARE chunk covering [0, start_block)
    the original chunks covering [start_block, end)

Because sparse chunks carry absolute positions in the header and the device
writes sequentially, flashing this image alone rewrites exactly the tail.

Usage:
    tools/sparse-tail.py IMAGE.simg OUTPUT.simg [--max-download 0x8000000]
    tools/sparse-tail.py IMAGE.simg OUTPUT.simg --block 3051321

The tail start must fall on a chunk boundary; if the simulated split lands
inside a chunk the script walks back to the previous boundary (rewriting a few
already-written blocks is harmless).
"""

import argparse
import struct
import sys

MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_FILL = 0xCAC2
CHUNK_DONT_CARE = 0xCAC3
CHUNK_CRC32 = 0xCAC4
BLOCK = 4096


def parse(path):
    f = open(path, "rb")
    hdr = f.read(28)
    (magic, major, minor, fhdr, chdr, blk, total_blks, total_chunks, csum) = \
        struct.unpack("<IHHHHIIII", hdr)
    if magic != MAGIC or fhdr != 28 or chdr != 12:
        sys.exit(f"{path}: not an Android sparse image")
    pos = 28
    chunks = []
    for i in range(total_chunks):
        f.seek(pos)
        c = f.read(12)
        if len(c) != 12:
            sys.exit(f"{path}: truncated at chunk {i}")
        ctype, _res, csz, tsz = struct.unpack("<HHII", c)
        fill = None
        if ctype == CHUNK_FILL:
            fill = struct.unpack("<I", f.read(4))[0]
        chunks.append(dict(pos=pos, type=ctype, csz=csz, tsz=tsz,
                           data=pos + 12, fill=fill))
        pos += tsz
    if pos != f.seek(0, 2):
        sys.exit(f"{path}: chunks end at {pos}, file is larger")
    if sum(c["csz"] for c in chunks) != total_blks:
        sys.exit(f"{path}: chunk blocks != header total_blks")
    return f, blk, total_blks, chunks


def merge_blocks(chunks):
    """libsparse merges adjacent FILL blocks; RAW blocks stay separate."""
    blocks = []
    block = 0
    for c in chunks:
        length = c["csz"] * BLOCK
        if (c["type"] == CHUNK_FILL and blocks
                and blocks[-1]["type"] == CHUNK_FILL
                and blocks[-1]["fill"] == c["fill"]
                and blocks[-1]["block"] + blocks[-1]["len"] == block):
            blocks[-1]["len"] += length
        else:
            blocks.append(dict(block=block, len=length,
                               type="fill" if c["type"] == CHUNK_FILL else "raw",
                               fill=c["fill"], data=c["data"]))
        block += c["csz"]
    return blocks


def last_segment_block(blocks, total_blks, limit):
    """Return the start block of the final segment per sparse_file_resparse()."""
    size = limit - 56  # 28-byte header + 2 chunk headers + trailing skip header
    idx = 0
    last_start = 0
    while idx < len(blocks):
        start_block = blocks[idx]["block"]
        last_start = start_block
        last_block = 0
        used = 0
        first_in_segment = True
        while idx < len(blocks):
            b = blocks[idx]
            count = 0
            if b["block"] > last_block:
                count += 12  # gap skip chunk
            last_block = b["block"] + b["len"] // BLOCK
            count += 12 + (b["len"] if b["type"] == "raw" else 4)
            if used + count > size:
                used += 12  # header for the end-of-segment skip
                if (first_in_segment or size - used > size // 8):
                    cut = ((size - used) // BLOCK) * BLOCK
                    if 0 < cut < b["len"]:
                        # Split b: the front part belongs to this segment and
                        # the remainder starts the next one.
                        blocks[idx] = dict(b)
                        blocks[idx]["block"] = b["block"] + cut // BLOCK
                        blocks[idx]["len"] = b["len"] - cut
                        if b["type"] == "raw":
                            blocks[idx]["data"] = b["data"] + cut
                break
            used += count
            first_in_segment = False
            idx += 1
        if blocks and idx < len(blocks) and blocks[idx]["block"] == start_block:
            return start_block  # no progress (cannot happen with sane limits)
    return last_start


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("output")
    ap.add_argument("--max-download", default="0x8000000",
                    help="device max-download-size (default 0x8000000)")
    ap.add_argument("--block", type=lambda x: int(x, 0),
                    help="tail start block (default: simulate the last segment)")
    args = ap.parse_args()

    f, blk, total_blks, chunks = parse(args.image)
    if blk != BLOCK:
        sys.exit(f"{args.image}: block size {blk}, only {BLOCK} supported")

    if args.block is None:
        start_block = last_segment_block(merge_blocks(chunks), total_blks,
                                         int(args.max_download, 0))
    else:
        start_block = args.block

    # Find the last chunk boundary at or before start_block.
    off = 0
    start = None
    for i, c in enumerate(chunks):
        if off + c["csz"] > start_block:
            break
        off += c["csz"]
        if off == start_block:
            start = i + 1
            break
    if start is None or start >= len(chunks):
        sys.exit(f"block {start_block} does not fall on a chunk boundary")
    start_block = off

    tail_blocks = total_blks - start_block
    hdr = struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLOCK, total_blks,
                      1 + (len(chunks) - start), 0)
    dont = struct.pack("<HHII", CHUNK_DONT_CARE, 0, start_block, 12)

    with open(args.output, "wb") as g:
        g.write(hdr)
        g.write(dont)
        f.seek(chunks[start]["pos"])
        remaining = chunks[-1]["pos"] + chunks[-1]["tsz"] - chunks[start]["pos"]
        while remaining:
            buf = f.read(min(1 << 20, remaining))
            if not buf:
                break
            g.write(buf)
            remaining -= len(buf)

    print(f"{args.output}: tail starts at block {start_block} "
          f"({start_block * BLOCK / 2**30:.2f} GiB prefix, "
          f"{tail_blocks * BLOCK / 2**30:.2f} GiB tail), "
          f"chunks {start}..{len(chunks) - 1}")


if __name__ == "__main__":
    main()
