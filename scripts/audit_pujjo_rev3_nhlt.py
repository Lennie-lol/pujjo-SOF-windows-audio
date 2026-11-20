#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Pujjo rev3 candidate table audit. Not an activation or approval tool."""
import argparse
import json
import struct
from pathlib import Path
from prepare_pujjo_ssp_candidate import ENDPOINT, WAVE, PCM, ROUTES, digest, build
from generate_pujjo_ssp_candidate import candidate, ALSA_REV, RECOVERY_SHA

def audit(data):
    if len(data) < 37 or data[:4] != b"NHLT" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("bad NHLT header")
    if sum(data) & 255 or data[36] != len(ROUTES):
        raise ValueError("bad checksum or endpoint count")
    pos = 37
    seen = set()
    routes = []
    for _ in range(data[36]):
        if len(data)-pos < ENDPOINT.size:
            raise ValueError("truncated endpoint")
        length, link, instance, vendor, device, rev, sub, kind, direction, port = ENDPOINT.unpack_from(data,pos)
        end = pos+length
        if length != ENDPOINT.size+7+WAVE.size+4+96 or end > len(data):
            raise ValueError("wrong endpoint size")
        if (link,instance,vendor,device,rev,sub,kind) != (3,0,0x8086,0xAE34,0,0,4):
            raise ValueError("unexpected SSP metadata")
        key=(port,direction)
        if key not in ROUTES or key in seen:
            raise ValueError("unexpected or duplicate endpoint")
        seen.add(key)
        at=pos+ENDPOINT.size
        if data[at:at+7] != bytes([2,0,0,0,0,0,1]):
            raise ValueError("unexpected capabilities or format count")
        at+=7
        if WAVE.unpack_from(data,at) != (0xfffe,2,48000,384000,8,32,22,24,3,PCM):
            raise ValueError("unexpected PCM format or channel mask")
        at+=WAVE.size
        if struct.unpack_from("<I",data,at)[0] != 96 or data[at+4:end] != candidate(port):
            raise ValueError("payload differs from fixed review candidate")
        routes.append({"ssp":port,"direction":"capture" if direction else "render",
                       "rate":48000,"channels":2,"container_bits":32,"valid_bits":24,
                       "payload_sha256":digest(data[at+4:end])})
        pos=end
    if pos != len(data) or seen != set(ROUTES):
        raise ValueError("incomplete or trailing records")
    return routes

def verify(folder):
    data=(folder/"candidate-only.nhlt").read_bytes()
    a=(folder/"ssp0-candidate.bin").read_bytes()
    b=(folder/"ssp1-candidate.bin").read_bytes()
    if (a,b)!=(candidate(0),candidate(1)):
        raise ValueError("standalone candidate blobs differ")
    expected,_=build((folder/"ssp0-reference.nhlt").read_bytes(),
                     (folder/"ssp1-reference.nhlt").read_bytes(),a,b)
    if data!=expected:
        raise ValueError("upstream comparison does not reproduce table")
    return {
        "status":"OFFLINE_SSP_CANDIDATE_VERIFIED_NOT_HARDWARE_QUALIFIED",
        "target":{"manufacturer":"Google","board":"Pujjo","revision":"rev3",
                  "bios":"MrChromebox-2609.0","windows_build":26300,
                  "dsp":"8086:54c8","i2c":"8086:54eb","codec":"RTL5682","amp":"RTL1019",
                  "native_nhlt":"unavailable (reported Win32 error 1168)"},
        "target_evidence":"user-reported read-only Windows inventory; not measured electrical evidence",
        "alsa_revision":ALSA_REV,"recovery_topology_sha256":RECOVERY_SHA,
        "table_sha256":digest(data),"routes":audit(data),
        "missing_routes":["DMIC0 capture: no qualified IPC4 gateway configuration"],
        "hardware_qualified":False,"activation_authorized":False,
        "remaining":["independent rev3 DMIC evidence","running firmware clock and reserved-bit review",
                     "translated resources and analog timing","physical DSP/codec/amp validation"]
    }

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("candidate_dir",type=Path)
    args=p.parse_args()
    report=verify(args.candidate_dir)
    (args.candidate_dir/"pujjo-rev3-audit.json").write_text(json.dumps(report,indent=2)+"\n")
    print("PASS: exact rev3-targeted SSP candidate audit; DMIC absent; no hardware approval")
