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
# Check both interrupt control paths, not just runtime dispatch.
for name in ("SofIpcEnableInterrupts", "SofIpcDisableInterrupts"):
    body=ipc.split("VOID\n"+name+"(",1)[1].split("\n/**",1)[0]
    assert "if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||" in body
    assert body.index("HipcCtl, 4u") < body.index("SofDspUpdateBits(")
    assert body.index("MTL_HFINTIPPTR, 4u") < body.index("rawPtr = SofDspRead32")
    assert body.index("MTL_DSP_REG_HfHIPCIE, 4u") < body.index("rawPtr = SofDspRead32")
    assert body.index("rawPtr == 0xFFFFFFFFu") < body.index("hfintipptr = rawPtr &")
    assert body.index("hfintipptr, 4u") < body.index("                         hfintipptr,")
# The ISR must validate all registers its DPC will need before masking.
for body in (isr, dpc):
    for register in ("HipcIda", "HipcTdr", "HipcTdd", "HipcTda", "HipcCtl"):
        assert body.index(register+", 4u") < body.index("SofDspRead32(")
    assert body.index("tdr == 0xFFFFFFFFu || ida == 0xFFFFFFFFu") < body.index("SofDspUpdateBits(")
assert isr.index("ptr == 0xFFFFFFFFu") < isr.index("ptr &= MTL_HFINTIPPTR_PTR_MASK")
print("PASS: bounded interrupt control and dispatch, unavailable-device doorbells, gated FW_READY and no unreviewed firmware retry")

# Only the registered interrupt DPC may consume hardware doorbells.
boot=loader.split("NTSTATUS\nSofStartFirmwareBoot(",1)[1]
assert "SofIpcDpcHandler(Ctx)" not in boot
assert "KeWaitForSingleObject(&Ctx->FwReadyEvent" in boot
assert "status != STATUS_SUCCESS || Ctx->IpcFaulted" in boot
assert "Ctx->IpcTxBusy = FALSE" not in boot
assert "Ctx->IpcFaulted = FALSE" not in boot
publish=dpc.split("if (bootReady || bootFault)",1)[1]
assert dpc.index("SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda") < dpc.index("Ctx->FwState = SOF_FW_BOOT_COMPLETE")
assert publish.index("KeAcquireSpinLock(&Ctx->IpcLock") < publish.index("Ctx->FwState = SOF_FW_BOOT_COMPLETE") < publish.index("KeReleaseSpinLock(&Ctx->IpcLock")
assert "bootReady && !Ctx->IpcFaulted" in publish
assert "else if (bootFault)" in publish
assert "KeSetEvent(&Ctx->FwReadyEvent" in publish.split("else if (bootFault)",1)[1]
assert "Restore DONE even for a BUSY-only IRQ" in dpc
print("PASS: single DPC consumer, acknowledged readiness, fault wakeup and bounded boot wait")

device=(root/"src/driver/Device.c").read_text()
d0=device.split("NTSTATUS\nSofEvtDeviceD0Entry(",1)[1].split("\n/**",1)[0]
assert d0.index("if (ipcFaulted ||") < d0.index("SofIpcEnableInterrupts(ctx)")
assert d0.index("return bootStatus;") < d0.index("WdfTimerStart(")
assert d0.count("ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;") == 1
assert d0.index("return STATUS_DEVICE_NOT_READY;",d0.index("bootStatus =")) < d0.index("WdfTimerStart(")
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in d0
print("PASS: D0 entry fails before starting timers when firmware is not ready")

# D0 must not run speculative audio graphs or reuse an unverified retained DSP.
assert "initialFwState != SOF_FW_BOOT_NOT_STARTED" in d0
assert "PreviousState != WdfPowerDeviceD3Final" in d0
assert d0.index("!ctx->HardwarePrepared") < d0.index("SofIpcEnableInterrupts(ctx)")
assert d0.index("ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;") < d0.index("ctx->CurrentPowerState = PowerDeviceD0;")
for forbidden in ("SofIpc4CreateHdmiPipeline(", "SofIpc4CreatePlaybackPipeline(",
                  "SofIpc4FreeInstanceId(", "SofHdaInitCodec(", "SofHdaInitDisplayCodec(",
                  "MTL_DSP2CXCTL_PRIMARY_CORE", "SofHdaSetD0i3(", "SofUaolInit("):
    assert forbidden not in d0, forbidden
assert "All on-demand pipelines verified" not in d0
print("PASS: cold boot ownership and no speculative audio activation on D0 entry")

ack=boot[boot.index("/* Wait for ROM IPC ACK"):boot.index("/* Acknowledge ROM")]
assert "ack == 0xFFFFFFFFu" in ack and "if (timeout == 2000u)" in ack
stop=boot[boot.index("stop_dma:"):boot.index("/* Check if FW entered")]
assert stop.index("!(ctlVal & SOF_HDA_SD_CTL_DMA_START)") < stop.index("goto dma_unconfirmed;", stop.index("if (timeout == 500u)")) < stop.index("SofFreeDmaBuffer")
assert "SofFreeDmaBuffer" not in boot.split("dma_unconfirmed:",1)[1]
assert "ExWaitForRundownProtectionRelease" in ipc
entry = ipc.split("NTSTATUS\nSofIpcSendMessage(",1)[1].split("\n}",1)[0]
assert entry.index("ExAcquireRundownProtection") < entry.index("SofIpcSendWhilePowered") < entry.index("ExReleaseRundownProtection")
assert "WdfTimerStop(ctx->JackPollTimer, TRUE)" in device

# SPIB teardown follows confirmed RUN clear and rejects inaccessible MMIO.
spib_stop = stop.split("if (Ctx->SpbCapOffset)",1)[1]
assert stop.index("if (timeout == 500u)") < stop.index("ULONG spbControl = readl(")
assert spib_stop.index("if (spbControl == 0xFFFFFFFFu)") < spib_stop.index("goto boot_failed;") < spib_stop.index("writel(0, spibAddr)")
assert "writel(spbControl & ~(1u << streamIndex)" in spib_stop
assert "writel(readl(" not in spib_stop
print("PASS: confirmed DMA stop precedes SPIB teardown; inaccessible SPIB fails closed")
