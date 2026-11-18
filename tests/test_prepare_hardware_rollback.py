#!/usr/bin/env python3
"""Source contract: PnP failure must unmap previously mapped MMIO BARs."""
from pathlib import Path
source=(Path(__file__).resolve().parents[1]/"src/driver/Device.c").read_text()
prep=source.split("SofEvtDevicePrepareHardware(",1)[1].split("\n/**",1)[0]
cleanup=source.split("static VOID SofUnmapFailedPreparedBars(",1)[1].split("\n/**",1)[0]
assert "MmUnmapIoSpace(Ctx->Bar4VirtualAddress, Ctx->Bar4Length)" in cleanup
assert "MmUnmapIoSpace(Ctx->Bar0VirtualAddress, Ctx->Bar0Length)" in cleanup
assert "Ctx->Bar0VirtualAddress = NULL" in cleanup
assert "Ctx->Bar4VirtualAddress = NULL" in cleanup
assert "ctx->Bar0Length < 0x80u || ctx->Bar4Length < sizeof(ULONG)" in prep
assert prep.index("goto PreparationFailed;") < prep.index("SofDiscoverHdaCaps(ctx)")
assert "if (!NT_SUCCESS(status)) goto PreparationFailed;" in prep
assert "SofUnmapFailedPreparedBars(ctx);" in prep
assert prep.index("PreparationFailed:") > prep.index("return STATUS_SUCCESS;")
assert "busInterface.SetBusData(" not in prep
print("PASS: failed WDF PrepareHardware unmaps any acquired MMIO BAR resources")
