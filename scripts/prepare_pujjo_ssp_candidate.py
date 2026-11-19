#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Prepare a review-only Windows-shaped NHLT from independent ALSA SSP records.

Reference: alsa-utils v1.2.13 topology/nhlt/intel/ssp-nhlt.c,
ssp/ssp-intel.h and ssp/ssp-internal.h. ALSA writes physical slot width
in WAVEFORMATEXTENSIBLE and leaves the subtype GUID zero. The Windows
driver requires a separate PCM container size, valid width and PCM GUID.
SSP1 requires a surrogate MCLK for the unmodified single-source encoder.
Clock field differences are reported, NEVER interpreted as hardware approval.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ENDPOINT = struct.Struct("<IBBHHHIBBB")
WAVE = struct.Struct("<HHIIHHHHI16s")
PCM = bytes.fromhex("0100000000001000800000aa00389b71")
FIELDS = ["gateway_attributes", "version", "size"] + [
    "ts_group_%d" % i for i in range(8)
] + ["ssc0", "ssc1", "sscto", "sspsp", "sstsa", "ssrsa", "ssc2",
     "sspsp2", "ssc3", "ssioc", "mdivctlr", "mdivrcnt", "mdivr"]
# The independent encoder's IO clock is chosen to reproduce the real SCR.
# Allow differences only in the source selectors and divider for its
# surrogate MCLK. Any other differing bit must be investigated.
CLOCK_MASK = {"mdivctlr": 0x00330000, "mdivr": 0x00000fff}
ROUTES = ((0, 0), (0, 1), (1, 0))
SLOTS = {0: 25, 1: 32}
DIV = {0: 16, 1: 8}
MAX_TABLE = 1024 * 1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pinned(path, expected, limit):
    with path.open("rb") as inp:
        data = inp.read(limit + 1)
    if len(data) > limit or digest(data) != expected.lower():
        raise ValueError("length or SHA-256 mismatch: " + str(path))
    return data


def reference_records(data, port):
    """Read unmodified ALSA NHLT, selecting exactly the expected SSP routes."""
    if port not in SLOTS or not 37 <= len(data) <= MAX_TABLE:
        raise ValueError("unsupported port or NHLT size")
    if data[:4] != b"NHLT" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("invalid NHLT header")
    if sum(data) & 255 and any(data[8:36]):
        raise ValueError("bad NHLT checksum")
    cursor = 37
    records = {}
    for _ in range(data[36]):
        if len(data) - cursor < ENDPOINT.size:
            raise ValueError("truncated endpoint")
        desc = ENDPOINT.unpack_from(data, cursor)
        length, link, instance, vendor, device, rev, sub, kind, direction, bus = desc
        end = cursor + length
        if length < ENDPOINT.size + 5 or end > len(data):
            raise ValueError("invalid endpoint length")
        pos = cursor + ENDPOINT.size
        cap = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if cap > end - pos - 1:
            raise ValueError("invalid endpoint capabilities")
        caps = data[pos:pos+cap]
        pos += cap
        count = data[pos]
        pos += 1
        selected = link == 3 and bus == port
        if selected and ((instance, vendor, device, rev, sub, kind) !=
                         (0, 0x8086, 0xAE34, 0, 0, 0)
                         or caps != bytes(2) or count != 1
                         or (port, direction) not in ROUTES):
            raise ValueError("unexpected ALSA SSP endpoint metadata")
        for _ in range(count):
            if end - pos < WAVE.size + 4:
                raise ValueError("truncated format")
            fmt = WAVE.unpack_from(data, pos)
            pos += WAVE.size
            size = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            if size > end - pos:
                raise ValueError("truncated format payload")
            blob = data[pos:pos+size]
            pos += size
            if selected:
                align = 2 * SLOTS[port] // 8
                expected = (0xfffe, 2, 48000, 48000 * align, align,
                            SLOTS[port], 22, SLOTS[port], 0, bytes(16))
                if fmt != expected or direction in records:
                    raise ValueError("unexpected or duplicate ALSA SSP format")
                records[direction] = blob
        if pos != end:
            raise ValueError("unconsumed endpoint bytes")
        cursor = end
    if cursor != len(data) or set(records) != ({0, 1} if port == 0 else {0}):
        raise ValueError("missing or unexpected SSP directions")
    return records


def decode(blob, port):
    if len(blob) != 96 or port not in SLOTS:
        raise ValueError("expected bare 96-byte SSP IPC4 v1.5 record")
    d = dict(zip(FIELDS, struct.unpack("<24I", blob)))
    if (d["version"], d["size"], d["mdivrcnt"]) != (0xEE000105, 96, 1):
        raise ValueError("wrong SSP record version, size or divider count")
    ssc0 = d["ssc0"]
    width = (ssc0 & 15) + 1 + (16 if ssc0 & (1 << 20) else 0)
    if (width != 24 or ((ssc0 >> 8) & 0xfff) + 1 != DIV[port]
            or (d["sspsp"] >> 16) & 0x3f != SLOTS[port]
            or d["sstsa"] & 0xff != 3 or d["ssrsa"] & 0xff != 3):
        raise ValueError("SSP sample width, BCLK divider, frame width or slot mask mismatch")
    return d


def compare(reference, candidate, port):
    a, b = decode(reference, port), decode(candidate, port)
    differences = {}
    for field in FIELDS:
        if a[field] == b[field]:
            continue
        if (a[field] ^ b[field]) & ~CLOCK_MASK.get(field, 0):
            raise ValueError("unexpected difference in " + field)
        differences[field] = {"encoder": a[field], "candidate": b[field]}
    # The proposed MCLK is a divide-by-two 38.4 MHz source. Its numerical
    # selector requires firmware/platform verification outside this tool.
    if not b["mdivctlr"] & 1 or b["mdivr"] & 0xfff:
        raise ValueError("candidate does not encode MCLK0 with divide-by-two")
    return differences


def build(encoder0, encoder1, candidate0, candidate1):
    references = {0: reference_records(encoder0, 0),
                  1: reference_records(encoder1, 1)}
    candidates = {0: candidate0, 1: candidate1}
    endpoints = []
    records = []
    for port, direction in ROUTES:
        candidate = candidates[port]
        reference = references[port][direction]
        changes = compare(reference, candidate, port)
        # Keep physical slot width in SSP registers, NOT the Windows PCM
        # container. Use the documented analog device type, not ALSA's 0.
        fmt = WAVE.pack(0xfffe, 2, 48000, 384000, 8, 32, 22, 24, 3, PCM)
        body = struct.pack("<I", 2) + bytes(2) + bytes([1]) + fmt
        body += struct.pack("<I", len(candidate)) + candidate
        endpoints.append(ENDPOINT.pack(ENDPOINT.size + len(body),
                         3, 0, 0x8086, 0xAE34, 0, 0, 4, direction, port) + body)
        records.append({"port": port, "direction": direction,
                        "encoder_blob_sha256": digest(reference),
                        "candidate_blob_sha256": digest(candidate),
                        "clock_differences": changes})
    body = bytes([len(endpoints)]) + b"".join(endpoints)
    table = bytearray(b"NHLT" + struct.pack("<I", 36 + len(body)) + bytes(28) + body)
    table[9] = -sum(table) & 255
    report = {"status": "CANDIDATE_ONLY_NOT_HARDWARE_APPROVED",
              "encoder_tables_sha256": [digest(encoder0), digest(encoder1)],
              "nhlt_sha256": digest(table), "records": records,
              "remaining": ["ADL-N source selectors and M/N state",
                            "reserved register review",
                            "actual clocks and codec/amp validation"]}
    return bytes(table), report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("encoder0", "encoder1", "candidate0", "candidate1"):
        p.add_argument("--" + name, type=Path, required=True)
        p.add_argument("--" + name + "-sha256", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    try:
        items = [pinned(getattr(args, n), getattr(args, n + "_sha256"),
                        MAX_TABLE if n.startswith("encoder") else 96)
                 for n in ("encoder0", "encoder1", "candidate0", "candidate1")]
        table, report = build(*items)
        # This is deliberately a binary candidate rather than the header
        # consumed by the kernel build. No reviewed macro is emitted.
        with args.output.open("xb") as out:
            out.write(table)
        with args.report.open("x", encoding="utf-8") as out:
            json.dump(report, out, indent=2)
            out.write("\n")
    except (ValueError, OSError) as exc:
        p.exit(1, str(exc) + "\n")


if __name__ == "__main__":
    main()
