#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract a digest-pinned SOF topology manifest for compile-time review."""
import argparse
import hashlib
from pathlib import Path
import struct


def extract(data, digest):
    if len(data) > 16 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("topology size or SHA-256 mismatch")
    pos = 0
    result = None
    manifest_seen = False
    while pos < len(data):
        if len(data) - pos < 36:
            raise ValueError("truncated topology header")
        magic, abi, _, kind, header, _, size, _, count = struct.unpack_from("<9I", data, pos)
        end = pos + header + size
        if magic != 0x41536F43 or abi != 5 or header != 36 or end > len(data):
            raise ValueError("invalid ALSA topology block")
        payload = data[pos+header:end]
        pos = end
        if kind != 8:
            continue
        if manifest_seen or count != 1 or size < 120:
            raise ValueError("missing IPC4 manifest header or repeated manifest")
        manifest_seen = True
        if struct.unpack_from("<I", payload)[0] != 112 or struct.unpack_from("<I", payload, 108)[0] != size-112:
            raise ValueError("ALSA manifest size mismatch")
        major, _, _, items = struct.unpack_from("<4H", payload, 112)
        if major != 3:
            raise ValueError("unsupported SOF topology ABI")
        cursor = 120
        for _ in range(items):
            if size-cursor < 8:
                raise ValueError("truncated SOF manifest item")
            tag, length = struct.unpack_from("<II", payload, cursor)
            cursor += 8
            if length > size-cursor:
                raise ValueError("truncated SOF manifest data")
            if tag == 1:
                if result is not None:
                    raise ValueError("duplicate NHLT")
                result = bytearray(payload[cursor:cursor+length])
            cursor += length
        if cursor != size:
            raise ValueError("unconsumed manifest data")
    if result is None or not 37 <= len(result) <= 1024*1024 or result[:4] != b"NHLT":
        raise ValueError("no usable topology NHLT")
    if struct.unpack_from("<I", result, 4)[0] != len(result):
        raise ValueError("NHLT size mismatch")
    # ALSA's NHLT compiler emits an unpublished ACPI header with zero OEM,
    # revision and checksum. Preserve every endpoint byte and finalize the checksum.
    if not any(result[8:36]):
        result[9] = -sum(result) & 255
    if sum(result) & 255:
        raise ValueError("invalid NHLT checksum")
    return bytes(result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=Path)
    parser.add_argument("sha256")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    table = extract(args.topology.read_bytes(), args.sha256)
    with args.output.open("x") as out:
        print("/* Source topology SHA-256 " + args.sha256 + " */", file=out)
        print("static const UCHAR PujjoTopologyNhlt[] = {", file=out)
        for start in range(0, len(table), 16):
            print("    " + ", ".join(f"0x{x:02x}" for x in table[start:start+16]) + ",", file=out)
        print("};", file=out)
