#!/usr/bin/env python3
"""Source-level interrupt/boot safety contracts. Not a live driver test."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ipc=(root/"src/driver/Ipc.c").read_text()
loader=(root/"src/driver/Loader.c").read_text()
isr=ipc.split("BOOLEAN\nSofIpcIsrCheck(",1)[1].split("\n/**",1)[0]
dpc=ipc.split("BOOLEAN\nSofIpcDpcHandler(",1)[1].split("\n/**",1)[0]
assert "if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc || SofDeviceRemoved(Ctx))" in isr
assert isr.count("pj_ipc4_mmio_span_valid(") >= 5
assert isr.index("pj_ipc4_mmio_span_valid(") < isr.index("SofDspRead32(")
assert "ptr > Ctx->Bar4Length" in isr
assert "MTL_DSP_IRQSTS > Ctx->Bar4Length - ptr" in isr
assert dpc.count("pj_ipc4_mmio_span_valid(") >= 5
assert "Ctx->FwState == SOF_FW_BOOT_IN_PROGRESS" in dpc
assert "Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS" in loader
assert loader.index("ctlVal |= SOF_HDA_SD_CTL_DMA_START") < loader.index("Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS")
assert "-community.ri" not in loader and "Falling back to community firmware" not in loader
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in loader
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in (root/"src/driver/Device.c").read_text()
# IRQ control is serialized with DIRQL dispatch and uses posted-write readback.
control=ipc.split("static BOOLEAN SofIpcSetIrqBits(",1)[1].split("\n}",1)[0]
assert control.index("SofDeviceRemoved(Ctx)") < control.index("SofDspRead32(")
assert control.index("pj_ipc4_mmio_span_valid(") < control.index("SofDspRead32(")
assert control.index("if (value == MAXULONG)") < control.index("SofDspWrite32(") < control.rindex("SofDspRead32(")
disable=ipc.split("BOOLEAN\nSofIpcDisableInterrupts(",1)[1].split("\n}",1)[0]
assert disable.index("WdfInterruptAcquireLock") < disable.index("Ctx->IrqArmed = FALSE")
assert disable.index("Ctx->IrqPending = FALSE") < disable.index("SofIpcMaskInterrupt(Ctx)")
assert disable.index("WdfInterruptReleaseLock") < disable.index("KeFlushQueuedDpcs")
# The ISR must validate all registers its DPC will need before masking.
for body in (isr, dpc):
    for register in ("HipcIda", "HipcTdr", "HipcTdd", "HipcTda", "HipcCtl"):
        assert body.index(register+", 4u") < body.index("SofDspRead32(")
    assert body.index("tdr == 0xFFFFFFFFu || ida == 0xFFFFFFFFu") < body.index("Ctx->IrqMaskFailed = !SofIpcSetIrqBits(" if body == isr else "SofDspWrite32(")
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
assert dpc.index("SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda") < dpc.index("Ctx->FwState = SOF_FW_BOOT_READY_RECEIVED")
assert publish.index("KeAcquireSpinLock(&Ctx->IpcLock") < publish.index("Ctx->FwState = SOF_FW_BOOT_READY_RECEIVED") < publish.index("KeReleaseSpinLock(&Ctx->IpcLock")
assert "bootReady && !Ctx->IpcFaulted" in publish
assert "Ctx->IpcFaulted = TRUE;" in publish
assert "Ctx->FwState = SOF_FW_BOOT_COMPLETE" not in dpc
fault = publish.split("} else {",1)[1]
assert fault.index("Ctx->FwState = SOF_FW_BOOT_FAILED;") < fault.index("KeSetEvent(&Ctx->FwReadyEvent")
assert "SofIpcRearmInterrupt(" not in dpc
assert dpc.index("SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda") < dpc.index("/* Flush the posted ACKs") < dpc.index("KeSetEvent(&Ctx->FwReadyEvent")
print("PASS: single DPC consumer, acknowledged readiness, fault wakeup and bounded boot wait")

device=(root/"src/driver/Device.c").read_text()
driver=(root/"src/driver/Driver.c").read_text()
assert "EvtDeviceD0EntryPostInterruptsEnabled = SofEvtDeviceD0Entry;" in driver
assert "EvtDeviceD0ExitPreInterruptsDisabled  = SofEvtDeviceD0Exit;" in driver
assert "interruptConfig.AutomaticSerialization = FALSE;" in device
d0=device.split("NTSTATUS\nSofEvtDeviceD0Entry(",1)[1].split("\n/**",1)[0]
assert d0.index("if (ipcFaulted ||") < d0.index("SofStartFirmwareBoot(ctx, NULL)")
assert d0.index("return bootStatus;") < d0.index("SofPublishAudioChild(") < d0.index("WdfTimerStart(")
assert d0.index("if (!NT_SUCCESS(portStatus))") < d0.index("NTSTATUS restoreStatus = SofRestoreBootController(ctx)") < d0.index("return NT_SUCCESS(restoreStatus) ? portStatus : restoreStatus;")
assert "SofSdwEnumerateChildren(" not in device and "SofHdaSendVerb(" not in device
assert "SofVerifyAdlNIpc4Candidate(rawFw, rawFwSize)" in boot
assert boot.index("SofVerifyAdlNIpc4Candidate(rawFw, rawFwSize)") < boot.index("SofAllocateOwnedDmaBuffer(")
assert boot.index("SofQueryBootedFirmwareVersion(Ctx, &fwInfo)") < boot.index("return STATUS_SUCCESS;")
assert d0.count("ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;") == 1
assert d0.index("return STATUS_DEVICE_NOT_READY;",d0.index("bootStatus =")) < d0.index("WdfTimerStart(")
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)" in d0
print("PASS: D0 entry fails before starting timers when firmware is not ready")

# D0 must not run speculative audio graphs or reuse an unverified retained DSP.
assert "initialFwState != SOF_FW_BOOT_NOT_STARTED" in d0
assert "PreviousState != WdfPowerDeviceD3Final" in d0
assert d0.index("!ctx->HardwarePrepared") < d0.index("SofStartFirmwareBoot(ctx, NULL)")
assert d0.index("ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;") < d0.index("ctx->CurrentPowerState = PowerDeviceD0;")
for forbidden in ("SofIpc4CreateHdmiPipeline(", "SofIpc4CreatePlaybackPipeline(",
                  "SofIpc4FreeInstanceId(", "SofHdaInitCodec(", "SofHdaInitDisplayCodec(",
                  "MTL_DSP2CXCTL_PRIMARY_CORE", "SofHdaSetD0i3(", "SofUaolInit("):
    assert forbidden not in d0, forbidden
assert "All on-demand pipelines verified" not in d0
print("PASS: cold boot ownership and no speculative audio activation on D0 entry")

ack=boot[boot.index("/* Wait for ROM IPC ACK"):boot.index("/* Acknowledge ROM")]
assert "ack == 0xFFFFFFFFu" in ack and "if (timeout == 2000u || SofRomPollExpired(pollStart, 2000u))" in ack
stop=boot[boot.index("stop_dma:"):boot.index("/* Re-read the ROM state AFTER")]
detach = loader.split("static NTSTATUS SofStopAndDetachBootStream(", 1)[1].split("\n}", 1)[0]
assert detach.index("!(ctl & SOF_HDA_SD_CTL_DMA_START)") < detach.index("if (attempt == 500u)") < detach.index("writel(0, Registers + SOF_HDA_ADSP_REG_SD_BDLPL)")
assert stop.index("SofStopAndDetachBootStream(") < stop.index("goto dma_unconfirmed;") < stop.index("SofFreeDmaBuffer")
assert "SofFreeDmaBuffer" not in boot.split("dma_unconfirmed:",1)[1]
assert "ExWaitForRundownProtectionRelease" in ipc
entry = ipc.split("NTSTATUS\nSofIpcSendMessage(",1)[1].split("\n}",1)[0]
assert entry.index("ExAcquireRundownProtection") < entry.index("SofIpcSendWhilePowered") < entry.index("ExReleaseRundownProtection")
assert device.index("SofDisconnectAudioChild(ctx)", device.index("SofEvtDeviceD0Exit(")) < device.index("powerStatus = SofPowerDownDsp(ctx)", device.index("SofEvtDeviceD0Exit("))

# SPIB teardown follows confirmed RUN clear and rejects inaccessible MMIO.
spib_stop = stop.split("if (Ctx->SpbCapOffset)",1)[1]
assert stop.index("SofStopAndDetachBootStream(") < stop.index("ULONG spbControl = readl(")
assert spib_stop.index("if (spbControl == 0xFFFFFFFFu)") < spib_stop.index("goto boot_failed;") < spib_stop.index("writel(0, spibAddr)")
assert "writel(spbControl & ~(1u << streamIndex)" in spib_stop
assert "writel(readl(" not in spib_stop
print("PASS: confirmed DMA stop precedes SPIB teardown; inaccessible SPIB fails closed")

# ROM ACK polling and runtime interrupt handling have distinct ownership.
isr_wrapper=device.split("BOOLEAN\nSofEvtInterruptIsr(",1)[1].split("\n}",1)[0]
dpc_wrapper=device.split("VOID\nSofEvtInterruptDpc(",1)[1].split("\n}",1)[0]
assert isr_wrapper.index("!ctx->IrqArmed") < isr_wrapper.index("SofIpcIsrCheck(ctx)")
assert dpc_wrapper.index("ctx->IrqArmed && ctx->IrqPending") < dpc_wrapper.index("SofIpcDpcHandler(ctx)")
assert dpc_wrapper.index("SofIpcDpcHandler(ctx)") < dpc_wrapper.index("SofIpcRearmInterrupt(ctx)")
assert "SofIpcEnableInterrupts(ctx)" not in d0
assert boot.index("SofIpcDisableInterrupts(Ctx)") < boot.index("SofDspWrite32(bar4, Ctx->HwDesc->HipcIdr, ipcHdr)")
run = boot.index("ctlVal |= SOF_HDA_SD_CTL_DMA_START")
assert boot.index("/* Acknowledge ROM */") < boot.rindex("incoming & Ctx->HwDesc->HipcTdrBusy", 0, run) < run
assert run < boot.index("!(ctlAfter & SOF_HDA_SD_CTL_DMA_START)") < boot.index("Ctx->FwState = SOF_FW_BOOT_IN_PROGRESS") < boot.index("SofIpcEnableInterrupts(Ctx)")
assert boot.count("SofIpcEnableInterrupts(Ctx)")==1
assert "incoming & Ctx->HwDesc->HipcTdrBusy" in boot
assert "doorbell & Ctx->HwDesc->HipcIdrBusy" in boot
exit_body=device.split("NTSTATUS\nSofEvtDeviceD0Exit(",1)[1].split("\n/**",1)[0]
assert "SofHdaSetD0i3(" not in device
assert exit_body.index("SofIpcDisableInterrupts(ctx)") < exit_body.index("SofIpcQuiesce(ctx)") < exit_body.index("powerStatus = SofPowerDownDsp(ctx)")
print("PASS: ROM polling exclusive ownership and D0 session invalidation")

# Compile and execute the real core-transition helper with a fake register.
# It tests readback, delayed acknowledgement, disappearance and timeout, not
# electrical power behavior or Windows ABI correctness.
import os, shlex, subprocess, tempfile
helper = 'static NTSTATUS SofCavsCoreTransition(' + loader.split(
    'static NTSTATUS SofCavsCoreTransition(', 1)[1].split('\n}', 1)[0] + '\n}\n'
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t ULONG;
typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_INVALID_BUFFER_SIZE -2
#define STATUS_IO_TIMEOUT -3
#define HDA_DSP_REG_ADSPCS 4u
typedef struct { int IsAce; } HW;
typedef struct { HW *HwDesc; void *Bar4VirtualAddress; ULONG Bar4Length; } CTX, *PSOF_DEVICE_CONTEXT;
static ULONG reg, pending, delay, reads, writes, stall_calls, vanish, stuck;
static int pj_ipc4_mmio_span_valid(ULONG size, ULONG offset, ULONG bytes) {
    return offset <= size && bytes <= size-offset;
}
static ULONG SofDspRead32(void *base, ULONG off) {
    assert(base && off == HDA_DSP_REG_ADSPCS);
    ++reads;
    if (vanish && reads >= vanish) return UINT32_MAX;
    if (delay && --delay==0 && !stuck) reg=pending;
    return reg;
}
static void SofDspWrite32(void *base, ULONG off, ULONG value) {
    assert(base && off == HDA_DSP_REG_ADSPCS);
    ++writes; pending=value; if (!stuck) delay=3;
}
static void KeStallExecutionProcessor(ULONG us) { assert(us==10u);++stall_calls; }
static void init(ULONG value) {reg=value;pending=delay=reads=writes=stall_calls=vanish=stuck=0;}
'''
cases = r'''
int main(void) {
    HW hw={0}; CTX ctx={&hw,&hw,8};
    init(0x200); assert(SofCavsCoreTransition(&ctx,1,1,0x200,0x200)==0);
    assert(reg==0x201 && writes==1 && stall_calls==2);
    init(0x201); assert(SofCavsCoreTransition(&ctx,1,1,0x200,0x200)==0 && writes==0);
    init(UINT32_MAX); assert(SofCavsCoreTransition(&ctx,1,1,0,0)==STATUS_DEVICE_NOT_READY && writes==0);
    init(0); vanish=2; assert(SofCavsCoreTransition(&ctx,1,1,0,0)==STATUS_DEVICE_NOT_READY && writes==1);
    init(0); stuck=1; assert(SofCavsCoreTransition(&ctx,1,1,0,0)==STATUS_IO_TIMEOUT && stall_calls==5000 && writes==1);
    init(0); assert(SofCavsCoreTransition(&ctx,1,1,0x200,0x200)==STATUS_IO_TIMEOUT && writes==1);
    init(0); ctx.Bar4Length=7; assert(SofCavsCoreTransition(&ctx,1,1,0,0)==STATUS_INVALID_BUFFER_SIZE && !reads && !writes);
    init(0); ctx.Bar4Length=8; hw.IsAce=1; assert(SofCavsCoreTransition(&ctx,1,1,0,0)==STATUS_DEVICE_NOT_READY && !reads && !writes);
    assert(SofCavsCoreTransition(0,1,1,0,0)==STATUS_DEVICE_NOT_READY);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pj_cavs_core_') as d:
    src=Path(d)/'test.c';exe=Path(d)/('test.exe' if os.name=='nt' else 'test')
    src.write_text(fixture+helper+cases)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                    *shlex.split(os.environ.get('CFLAGS','')),str(src),'-o',str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=10)
core_start=boot.split('/* Match the bundled cAVS reference:',1)[1].split('/* Wait for ROM IPC ACK',1)[0]
assert core_start.index('SofCavsCoreTransition(Ctx, reset, 0') < core_start.index('SofCavsCoreTransition(Ctx, stall, 0')
assert boot.index('SofStartIccmaxDma(Ctx, &iccmax)') < boot.index('SofCavsCoreTransition(Ctx, HDA_DSP_ADSPCS_SPA_MASK(1)') < boot.index('SofDspWrite32(bar4, Ctx->HwDesc->HipcIdr, ipcHdr)')
assert boot.index('SofStopIccmaxDma(Ctx, &iccmax)') < boot.index('KeWaitForSingleObject(&Ctx->FwReadyEvent')
iccmax=loader.split('static NTSTATUS SofStopIccmaxDma(',1)[1].split('static NTSTATUS SofQueryBootedFirmwareVersion',1)[0]
assert iccmax.index('SofStopAndDetachBootStream(') < iccmax.index('Dma->Published = FALSE;') < iccmax.index('SofFreeDmaBuffer(')
print('PASS: cAVS register transitions reject timeout, missing acknowledgement, and disappearing MMIO')

# Execute actual descriptor readback logic against host-only MMIO doubles.
address_helper = 'static NTSTATUS SofLoaderDmaAddressesMatch(' + loader.split(
    'static NTSTATUS SofLoaderDmaAddressesMatch(', 1)[1].split('\n}', 1)[0] + '\n}\n'
address_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t ULONG;
typedef unsigned char *PUCHAR;
typedef int NTSTATUS;
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define SOF_HDA_ADSP_REG_SD_BDLPL 0u
#define SOF_HDA_ADSP_REG_SD_BDLPU 4u
static unsigned char registers[8];
static ULONG low, high, reads;
static ULONG readl(PUCHAR address) {
    ++reads;
    if(address==registers) return low;
    assert(address==registers+4);
    return high;
}
'''
address_cases = r'''
int main(void) {
    assert(SofLoaderDmaAddressesMatch(NULL,0,0)==STATUS_DEVICE_NOT_READY && reads==0);
    low=0x123480u; high=2;
    assert(SofLoaderDmaAddressesMatch(registers,low,high)==0 && reads==2);
    assert(SofLoaderDmaAddressesMatch(registers,0,0)==STATUS_DEVICE_NOT_READY);
    assert(SofLoaderDmaAddressesMatch(registers,low,0)==STATUS_DEVICE_NOT_READY);
    assert(SofLoaderDmaAddressesMatch(registers,0,high)==STATUS_DEVICE_NOT_READY);
    low=MAXULONG;
    assert(SofLoaderDmaAddressesMatch(registers,low,high)==STATUS_DEVICE_NOT_READY);
    low=0; high=MAXULONG;
    assert(SofLoaderDmaAddressesMatch(registers,low,high)==STATUS_DEVICE_NOT_READY);
    high=0;
    assert(SofLoaderDmaAddressesMatch(registers,0,0)==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pj_loader_bdl_') as d:
    src=Path(d)/'test.c';exe=Path(d)/('test.exe' if os.name=='nt' else 'test')
    src.write_text(address_fixture+address_helper+address_cases)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                    *shlex.split(os.environ.get('CFLAGS','')),str(src),'-o',str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=10)
assert boot.index('dmaConfigured = TRUE;') < boot.index('SofLoaderDmaAddressesMatch(streamRegs,') < boot.index('SofStartIccmaxDma(Ctx, &iccmax)')
assert detach.index('writel(0, Registers + SOF_HDA_ADSP_REG_SD_BDLPU)') < detach.index('SofLoaderDmaAddressesMatch(Registers, 0, 0)') < detach.rindex('SofCodeLoaderReadControl(Registers)')
assert stop.index('SofStopAndDetachBootStream(') < stop.index('dmaConfigured = FALSE;')
assert 'goto dma_unconfirmed;' in stop.split('SofStopAndDetachBootStream(',1)[1].split('dmaConfigured = FALSE;',1)[0]
assert iccmax.index('SofStopAndDetachBootStream(Ctx, Dma->Registers, &Dma->Bdl, &dmaStatus)') < iccmax.index('Dma->Published = FALSE;')
iccmax_start=loader.split('static NTSTATUS SofStartIccmaxDma(',1)[1].split('static NTSTATUS SofStopIccmaxDma(',1)[0]
assert iccmax_start.index('SofLoaderDmaAddressesMatch(Dma->Registers,') < iccmax_start.index('SofCodeLoaderWriteControl(Dma->Registers, SOF_HDA_SD_CTL_DMA_START)')
print('PASS: loader and ICCMAX BDL publication and detachment readback')
