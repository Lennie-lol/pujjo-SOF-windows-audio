#!/usr/bin/env python3
"""Read-only PnP discovery regression: not a physical test."""
from pathlib import Path
src=(Path(__file__).resolve().parents[1]/"src/driver/Device.c").read_text()
caps=src.split("VOID SofDiscoverHdaCaps(",1)[1].split("\n/**",1)[0]
prep=src.split("SofEvtDevicePrepareHardware(",1)[1].split("\n/**",1)[0]
assert "writel(" not in caps and "WRITE_REGISTER_" not in caps
assert "busInterface.SetBusData(" not in prep
assert "PCI Command updated" not in prep
assert "writel(" not in prep and "WRITE_REGISTER_" not in prep
assert "SofHardwareDevelopmentAllowed()" in prep
assert prep.index("!SofHardwareDevelopmentAllowed()") < prep.index("WdfFdoQueryForInterface")
assert "SofDiscoverHdaCaps(ctx)" in prep
assert "SofIpcInitialize(ctx)" in prep
assert "no CRST writes" in prep
assert "ctx->Bar0Length < SOF_HDA_WAKESTS + sizeof(USHORT)" in prep
print("PASS: PCI/HDA resource discovery is read-only and checks codec status bounds")
