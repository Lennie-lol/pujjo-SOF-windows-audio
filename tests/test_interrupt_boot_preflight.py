#!/usr/bin/env python3
"""Source-level interrupt/boot safety contracts. Not a live driver test."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ipc=(root/"src/driver/Ipc.c").read_text()
loader=(root/"src/driver/Loader.c").read_text()
isr=ipc.split("BOOLEAN\nSofIpcIsrCheck(",1)[1].split("\n/**",1)[0]
dpc=ipc.split("VOID\nSofIpcDpcHandler(",1)[1].split("\n/**",1)[0]
assert "if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)" in isr
assert isr.count("pj_ipc4_mmio_span_valid(") >= 5
assert isr.index("pj_ipc4_mmio_span_valid(") < isr.index("SofDspRead32(")
assert "ptr > Ctx->Bar4Length" in isr
assert "MTL_DSP_IRQSTS > Ctx->Bar4Length - ptr" in isr
assert dpc.count("pj_ipc4_mmio_span_valid(") >= 5
assert "Ctx->FwState == SOF_FW_BOOT_IN_PROGRESS" in dpc
assert "Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS" in loader
assert loader.index("Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS") < loader.index("ctlVal |= SOF_HDA_SD_CTL_DMA_START")
assert "-community.ri" not in loader and "Falling back to community firmware" not in loader
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in loader
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in (root/"src/driver/Device.c").read_text()
print("PASS: bounded ISR register accesses, gated FW_READY and no unreviewed firmware retry")
