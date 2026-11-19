#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Translate the Pujjo recovery topology's SSP records to ALSA topology2 input."""
import hashlib
from pathlib import Path
import struct
import sys

SOURCE = "9ca138df2d471e9da3699566d2131a56cb9a0bc3274b074e7cc78e668ff5c697"


def translate(data):
    if hashlib.sha256(data).hexdigest() != SOURCE:
        raise ValueError("expected Pujjo topology from Nissa recovery 16836.33.0")
    pos = 0
    links = {}
    while pos < len(data):
        _, _, _, kind, hdr, _, size, _, count = struct.unpack_from("<9I", data, pos)
        ptr = pos + hdr
        pos = ptr + size
        if kind != 10:
            continue
        for _ in range(count):
            base = ptr
            fixed = struct.unpack_from("<I", data, base)[0]
            private = struct.unpack_from("<I", data, base + fixed - 4)[0]
            ptr += fixed + private
            name = data[base+8:base+52].split(b"\0")[0].decode()
            if name not in ("SSP0-Codec", "SSP1-Codec"):
                continue
            hw = struct.unpack_from("<III6BH7I", data, base+676)
            if hw[2:10] != (1, 0, 0, 0, 1, 1, 1, 0):
                raise ValueError("unsupported I2S framing or clock ownership")
            vendor = {}
            cur = base + fixed
            while cur < ptr:
                length, typ, entries = struct.unpack_from("<3I", data, cur)
                if typ in (2, 4, 5):
                    vendor.update(struct.iter_unpack("<II", data[cur+12:cur+length]))
                cur += length
            port = vendor[155]
            fields = dict(id=port, dai_index=port, name='"'+name+'"',
                          direction='"duplex"' if port == 0 else '"playback"',
                          default_hw_conf_id=hw[1], sample_bits=vendor[502],
                          bclk_delay=vendor[500], mclk_id=vendor[501],
                          clks_control=vendor[504], frame_pulse_width=vendor[503],
                          tdm_padding_per_slot='"true"' if vendor[505] else '"false"',
                          io_clk=f"$PUJJO_SSP{port}_IO_CLK", version="$PUJJO_SSP_BLOB_VERSION")
            if vendor[506]:
                raise ValueError("untranslated clock quirk")
            fmt = dict(zip(("mclk_freq","bclk_freq","fsync_freq","tdm_slots",
                            "tdm_slot_width","tx_slots","rx_slots"), hw[10:]))
            fmt.update(id=hw[1], name='"'+name+'-I2S"', format='"I2S"', mclk='"codec_mclk_in"',
                       bclk='"codec_consumer"', fsync='"codec_consumer"',
                       bclk_invert='"false"', fsync_invert='"false"')
            links[port] = (fields, fmt)
    lines = ["# Pujjo/Nissa 16836.33.0 IPC3 source SHA-256 " + SOURCE,
             "# Encoder input only. Platform clock sources and blob version require review.",
             "Object.Dai.SSP ["]
    for port in (0, 1):
        fields, fmt = links[port]
        lines += ["    {", *[f"        {k} {v}" for k,v in fields.items()],
                  "        Object.Base.hw_config.1 {",
                  *[f"            {k} {v}" for k,v in fmt.items()], "        }", "    }"]
    return "\n".join(lines + ["]", ""])


if __name__ == "__main__":
    print(translate(Path(sys.argv[1]).read_bytes()), end="")
