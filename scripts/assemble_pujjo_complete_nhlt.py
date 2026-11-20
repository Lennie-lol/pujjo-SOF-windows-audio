#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Assemble the four supported Pujjo routes, for offline review only."""
import argparse
import json
import struct
from pathlib import Path

from prepare_pujjo_ssp_candidate import ENDPOINT, WAVE, PCM, digest
from audit_pujjo_rev3_nhlt import audit as audit_ssp, verify as verify_ssp
from audit_pujjo_dmic_reference import audit as audit_dmic

def checksum(data):
    table = bytearray(data)
    struct.pack_into("<I", table, 4, len(table))
    table[9] = 0
    table[9] = -sum(table) & 255
    return bytes(table)

def assemble(ssp, dmic):
    audit_ssp(ssp)
    audit_dmic(dmic)
    # Generic capability: do not invent physical microphone array geometry.
    # Intel PDM device ID AE20 from ALSA nhlt.h, device type 1 per Windows
    # NHLT lookup contract. Zero channel mask denotes raw microphone slots.
    fmt = WAVE.pack(0xfffe, 4, 48000, 768000, 16, 32, 22, 32, 0, PCM)
    body = struct.pack("<I", 2) + bytes(2) + bytes([1]) + fmt
    body += struct.pack("<I", len(dmic)) + dmic
    ep = ENDPOINT.pack(ENDPOINT.size + len(body), 2, 0, 0x8086,
                       0xae20, 0, 0, 1, 1, 0) + body
    return checksum(ssp[:36] + bytes([4]) + ssp[37:] + ep)

def audit(data, ssp, dmic):
    # Compare complete serialization against the two separately checked
    # sources, not merely against a repaired ACPI checksum.
    if len(data) < 37 or data[:4] != b"NHLT" or sum(data) & 255:
        raise ValueError("invalid complete NHLT header or checksum")
    if struct.unpack_from("<I", data, 4)[0] != len(data) or data[36] != 4:
        raise ValueError("wrong complete table length or route count")
    if data != assemble(ssp, dmic):
        raise ValueError("complete candidate differs from reviewed inputs")
    at = 37
    routes = []
    for _ in range(4):
        length, link, inst, vendor, device, rev, sub, kind, direction, bus = ENDPOINT.unpack_from(data, at)
        end = at + length
        caps = at + ENDPOINT.size
        if data[caps:caps+7] != bytes([2,0,0,0,0,0,1]):
            raise ValueError("unexpected endpoint capabilities")
        fmt_at = caps + 7
        fmt = WAVE.unpack_from(data, fmt_at)
        n = struct.unpack_from("<I", data, fmt_at+WAVE.size)[0]
        blob = data[fmt_at+WAVE.size+4:end]
        if len(blob) != n or end > len(data):
            raise ValueError("invalid payload bounds")
        routes.append({"link": "DMIC" if link==2 else "SSP",
                       "bus": bus, "direction": "capture" if direction else "render",
                       "channels": fmt[1], "rate": fmt[2], "container_bits": fmt[5],
                       "valid_bits": fmt[7], "payload_bytes": n,
                       "payload_sha256": digest(blob)})
        at = end
    if at != len(data):
        raise ValueError("trailing bytes")
    return routes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate_dir", type=Path)
    args = parser.parse_args()
    folder = args.candidate_dir
    ssp_report = verify_ssp(folder)
    ssp = (folder/"candidate-only.nhlt").read_bytes()
    dmic = (folder/"dmic/dmic0-reference.bin").read_bytes()
    dm_report = audit_dmic(dmic)
    data = assemble(ssp, dmic)
    routes = audit(data, ssp, dmic)
    # Mutations must fail even when the ACPI checksum is recomputed.
    for offset in (36,37,37+4,37+ENDPOINT.size, len(ssp)+4,
                   len(ssp)+ENDPOINT.size+7+2, len(data)-1):
        bad = bytearray(data)
        bad[offset] ^= 1
        try:
            audit(checksum(bad), ssp, dmic)
        except ValueError:
            continue
        raise ValueError("accepted mutated complete table at %d" % offset)
    for bad in (data[:-4], data+bytes(4)):
        try:
            audit(checksum(bad), ssp, dmic)
        except ValueError:
            continue
        raise ValueError("accepted truncated or extended complete table")
    report = {"status": "FOUR_ROUTE_REPLACEMENT_CANDIDATE_NOT_HARDWARE_QUALIFIED",
              "target": ssp_report["target"],
              "alsa_revision": ssp_report["alsa_revision"],
              "recovery_topology_sha256": ssp_report["recovery_topology_sha256"],
              "ssp_table_sha256": digest(ssp), "dmic_reference": dm_report,
              "table_sha256": digest(data), "table_bytes": len(data),
              "routes": routes, "hardware_qualified": False,
              "activation_authorized": False,
              "remaining": ["SSP clock selector/reserved bits and analog timing",
                            "physical microphone geometry and running DSP interpretation",
                            "signed diagnostic and physical testing"]}
    (folder/"complete-candidate-only.nhlt").write_bytes(data)
    (folder/"complete-candidate-review.json").write_text(json.dumps(report,indent=2)+"\n")
    print("PASS: complete four-route NHLT candidate assembled and audited; %d bytes" % len(data))

if __name__ == "__main__":
    main()
