#!/usr/bin/env python3
"""Tests for descriptor bounds, types and safe failure behavior."""
from pathlib import Path
import importlib.util
repo = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('pujjo_acpi', repo/'tools/analyze_acpi_audio.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)
# Resource descriptors are deliberately tiny isolated fixtures, not real ACPI tables.
i2c = bytes([0x8e,0x1e,0, 1,0,1,2,0,0,1,6,0,0x80,0x1a,0x06,0,0x1a,0])+b'\\_SB.PCI0.I2C3\0'
irq = bytes([0x8c,0x25,0,1,0,1,0,5,0,0,0,0,0,0,0x17,0,0,0x19,0,0x28,0,0,0,0x57,0])+b'\\_SB.PCI0.GPIO\0'
gpio = bytes([0x8c,0x25,0,1,1,1,0,2,0,0,0,0,0,0,0x17,0,0,0x19,0,0x28,0,0,0,0x4b,0])+b'\\_SB.PCI0.GPIO\0'
for r in [i2c,irq,gpio]:
    assert p.decode_resources(r+b'\x79\0')
assert p.decode_resources(i2c+b'\x79\0')[0]['speed_hz'] == 400000
assert p.decode_resources(i2c+b'\x79\0')[0]['address'] == 0x1a
assert p.decode_resources(irq+b'\x79\0')[0]['pin'] == 87
assert p.decode_resources(gpio+b'\x79\0')[0]['pin'] == 75
assert p.decode_resources(gpio+b'\x79\0')[0]['type'] == 'gpio_io'
try:
    p.decode_resources(gpio[:16])
    raise AssertionError('truncation accepted')
except p.AcpiDecodeError: pass
print('PASS: 9 ACPI parser checks')
