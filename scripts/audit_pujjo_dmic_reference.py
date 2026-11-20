#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Audit unmodified ALSA DMIC serialization; no kernel header is emitted."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

CLOCK = 38_400_000

def audit(data):
    if len(data) < 232 or len(data) > 8192 or len(data) % 4:
        raise ValueError("bad DMIC payload length")
    attrs, ts0, ts1, ts2, ts3, delay, mask, out0, out1, pdm = struct.unpack_from("<10I", data)
    if (attrs, ts0, ts1, ts2, ts3, delay, mask, out0, out1, pdm) != (
            1, 0xffff1010, 0xffffffff, 0xffffffff, 0xffffffff, 16, 3, 0x3a0003, 0, 3):
        raise ValueError("unexpected DMIC0 FIFO or controller routing")
    pos = 40
    records = []
    for controller in range(2):
        if len(data)-pos < 96:
            raise ValueError("truncated controller or FIR header")
        cic, cfg, reserved, mic, pdmsm, reuse, r1, r2 = struct.unpack_from("<8I", data, pos)
        if cic != 0xc001 or mic & ~0xff00 != 3 or any((reserved,pdmsm,reuse,r1,r2)):
            raise ValueError("unexpected controller settings")
        if cfg & ~0x0f00ff00:
            raise ValueError("unexpected CIC register fields")
        divider = ((mic >> 8) & 255) + 2
        decim = ((cfg >> 8) & 255) + 1
        shift = ((cfg >> 24) & 15) - 8
        if not 5 <= decim <= 31 or not -8 <= shift <= 4 or not 2_400_000*divider <= CLOCK <= 4_800_000*divider:
            raise ValueError("CIC or clock outside recovery constraints")
        if 5*(divider//2) < 2*divider or 5*((divider+1)//2) > 3*divider:
            raise ValueError("clock duty outside recovery constraints")
        firs=[]
        for off in (32,64):
            ctl, fcfg, *rest = struct.unpack_from("<8I",data,pos+off)
            if ctl != 0x91 or any(rest) or fcfg & ~0x001f0fff:
                raise ValueError("unexpected FIR register fields")
            n=(fcfg&255)+1
            ratio=((fcfg>>16)&31)+1
            if n>250 or not 2<=ratio<=20 or ((fcfg>>8)&15)>8:
                raise ValueError("bad FIR length, shift or decimation")
            if CLOCK != 48000*divider*decim*ratio:
                raise ValueError("clock and decimation do not produce 48 kHz")
            firs.append((n,ratio))
        pos+=96
        for n,_ in firs:
            if 4*n>len(data)-pos:
                raise ValueError("truncated FIR taps")
            coefficients=struct.unpack_from("<%dI"%n,data,pos)
            if any(x>0xfffff for x in coefficients) or not any(coefficients):
                raise ValueError("invalid FIR coefficients")
            pos+=4*n
        records.append({"controller":controller,"clock_divider":divider,
                        "pdm_clock_hz":CLOCK/divider,"cic_decimation":decim,
                        "cic_shift":shift,"fir_lengths":[x[0] for x in firs],
                        "fir_decimations":[x[1] for x in firs]})
    if pos!=len(data) or records[0]["pdm_clock_hz"]!=records[1]["pdm_clock_hz"]:
        raise ValueError("trailing data or inconsistent clocks")
    return {"status":"ENCODER_REFERENCE_ONLY_NOT_HARDWARE_QUALIFIED",
            "candidate_sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data),
            "sample_rate":48000,"channels":4,"container_bits":32,
            "reference_io_clock":CLOCK,"controllers":records,
            "one_ms_pcm_bytes":768,"activation_authorized":False,
            "clock_on_delay_ms":delay,"recovery_unmute_ramp_ms":200}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference",type=Path)
    p.add_argument("report",type=Path)
    args=p.parse_args()
    data=args.reference.read_bytes()
    report=audit(data)
    # Negative tests operate on the actual independently generated blob.
    for offset in (0,4,24,28,32,36,40,44,52,72,76):
        bad=bytearray(data)
        bad[offset]^=0x80
        try:
            audit(bad)
        except ValueError:
            continue
        raise ValueError("accepted altered record at %d"%offset)
    for bad in (data[:-4], data+b"\0"*4):
        try:
            audit(bad)
        except ValueError:
            continue
        raise ValueError("accepted corrupt payload length")
    args.report.write_text(json.dumps(report,indent=2)+"\n")
    print("PASS: DMIC0 independent encoder reference, four-channel geometry and malformed-payload rejection")

if __name__=="__main__":
    main()
