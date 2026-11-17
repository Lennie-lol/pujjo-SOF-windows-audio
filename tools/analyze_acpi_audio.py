#!/usr/bin/env python3
"""Offline, read-only ACPI audio resource inspection for a PUJJO SSDT.

Limited decoder: recognizes literal HID/CRS definitions and standard I2C/GPIO
resource descriptors. It is NOT an AML interpreter and never evaluates methods,
writes registers, or opens a Windows device. Reject ambiguous declarations.
"""
import argparse
import hashlib
import json
from pathlib import Path


class AcpiDecodeError(ValueError):
    pass


def u16(b, pos):
    return int.from_bytes(b[pos:pos + 2], "little")


def decode_resources(data):
    out = []
    offset = 0
    while offset < len(data):
        tag = data[offset]
        if tag & 0x80:
            if offset + 3 > len(data):
                raise AcpiDecodeError("truncated large resource header")
            length = u16(data, offset + 1)
            end = offset + 3 + length
            if end > len(data):
                raise AcpiDecodeError("resource exceeds buffer")
            r = data[offset:end]
            if tag == 0x8e:
                if length < 15 or r[5] != 1 or r[3] not in (1, 2):
                    raise AcpiDecodeError("unsupported I2C resource")
                source = r[18:].split(b'\0', 1)[0].decode('ascii')
                out.append({
                    "type": "i2c", "address": u16(r, 16),
                    "speed_hz": int.from_bytes(r[12:16], 'little'),
                    "addressing": "10-bit" if r[7] & 1 else "7-bit",
                    "controller": source,
                })
            elif tag == 0x8c:
                if length < 22 or r[3] != 1 or r[4] not in (0, 1):
                    raise AcpiDecodeError("unsupported GPIO resource")
                pin_offset = u16(r, 14)
                source_offset = u16(r, 17)
                if pin_offset < 23 or pin_offset + 2 > len(r) or source_offset >= len(r):
                    raise AcpiDecodeError("invalid GPIO offsets")
                source = r[source_offset:].split(b'\0', 1)[0].decode('ascii')
                out.append({
                    "type": "gpio_int" if r[4] == 0 else "gpio_io",
                    "pin": u16(r, pin_offset),
                    "flags_raw": u16(r, 7),
                    "controller": source,
                })
            offset = end
        else:
            length = tag & 7
            end = offset + 1 + length
            if end > len(data):
                raise AcpiDecodeError("small resource exceeds buffer")
            if tag == 0x79:
                break
            offset = end
    return out


def aml_pkg_length(data, offset):
    b = data[offset]
    count = b >> 6
    if offset + count >= len(data):
        raise AcpiDecodeError("truncated AML PkgLength")
    if count == 0:
        return b & 0x3f, 1
    value = b & 0x0f
    for i in range(count):
        value |= data[offset + i + 1] << (4 + 8 * i)
    return value, count + 1


def extract_named_crs(data, hid):
    marker = b'_HID\x0d' + hid.encode('ascii') + b'\x00'
    if data.count(marker) != 1:
        raise AcpiDecodeError(f"expected exactly one HID {hid}, found {data.count(marker)}")
    marker_at = data.index(marker)
    # Only accept literal _CRS Buffer objects adjacent to the known device.
    # Stop before another hardware ID; this is *not* a general AML namespace parser.
    next_hid = data.find(b'_HID\x0d', marker_at + len(marker))
    bound = min(marker_at + 350, next_hid if next_hid != -1 else len(data))
    search = b'\x08_CRS\x11'
    starts = []
    i = marker_at + len(marker)
    while i < bound:
        hit = data.find(search, i, bound)
        if hit < 0:
            break
        starts.append(hit + len(search))
        i = hit + 1
    if len(starts) != 1:
        raise AcpiDecodeError(f"expected exactly one literal _CRS for {hid}")
    start = starts[0]
    package_len, bytes_len = aml_pkg_length(data, start)
    size_index = start + bytes_len
    if data[size_index] == 0x0b:
        size = u16(data, size_index + 1)
        size_field_len = 3
    elif data[size_index] == 0x0a:
        size = data[size_index + 1]
        size_field_len = 2
    else:
        raise AcpiDecodeError("unsupported AML buffer length")
    raw_at = size_index + size_field_len
    if bytes_len + size_field_len + size != package_len:
        raise AcpiDecodeError("Buffer length mismatch")
    if raw_at + size > bound:
        raise AcpiDecodeError("ACPI buffer crosses device boundary")
    return decode_resources(data[raw_at:raw_at + size]), data[marker_at:bound]


def analyze(ssdt):
    if len(ssdt) < 36 or ssdt[:4] != b'SSDT':
        raise AcpiDecodeError("not an SSDT ACPI table")
    declared_len = int.from_bytes(ssdt[4:8], 'little')
    if declared_len != len(ssdt) or sum(ssdt) % 256 != 0:
        raise AcpiDecodeError("ACPI SSDT length/checksum invalid")
    codec, codec_aml = extract_named_crs(ssdt, 'RTL5682')
    amp, amp_aml = extract_named_crs(ssdt, 'RTL1019')
    if not any(r.get('type') == 'i2c' for r in codec):
        raise AcpiDecodeError("RT5682 lacks I2C resource")
    if any(r.get('type') == 'i2c' for r in amp):
        raise AcpiDecodeError("unexpected amplifier I2C resource")
    return {
        'source': 'raw SSDT ACPI table (static extraction only)',
        'ssdt_sha256': hashlib.sha256(ssdt).hexdigest(),
        'codec': {'hid': 'RTL5682', 'resources': codec,
                  'dsd_irq_gpios_present': b'irq-gpios' in codec_aml,
                  'dsd_realtek_jd_src_present': b'realtek,jd-src' in codec_aml},
        'amplifier': {'hid': 'RTL1019', 'resources': amp,
                      'dsd_sdb_gpios_present': b'sdb-gpios' in amp_aml},
        'hardware_access_performed': False,
        'caveat': 'ACPI table decoding does not prove runtime GPIO polarity, SSP routing, firmware compatibility, or speaker safety.'
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ssdt', required=True, type=Path)
    p.add_argument('--json-output', type=Path)
    a = p.parse_args()
    result = analyze(a.ssdt.read_bytes())
    payload = json.dumps(result, indent=2)
    if a.json_output:
        a.json_output.write_text(payload + '\n', encoding='utf-8')
    print(payload)


if __name__ == '__main__':
    main()
