#!/usr/bin/env python3
"""Exercise 54C8 IRQ gates and lifecycle with register and KMDF doubles."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
ipc = (root / "src/driver/Ipc.c").read_text()
device = (root / "src/driver/Device.c").read_text()
control = ipc[ipc.index("static BOOLEAN SofIpcSetIrqBits("):ipc.index("/**\n * @brief Captures and persists")]
isr = "BOOLEAN\nSofIpcIsrCheck(" + ipc.split("BOOLEAN\nSofIpcIsrCheck(", 1)[1].split("\n/**", 1)[0]
callbacks = device[device.index("NTSTATUS\nSofEvtInterruptEnable("):]
fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define _In_
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define FALSE 0
#define TRUE 1
#define STATUS_SUCCESS 0
#define MAXULONG UINT32_MAX
typedef int BOOLEAN, NTSTATUS, KIRQL;
typedef uint32_t ULONG;
typedef void VOID;
enum { HDA_DSP_REG_ADSPIS, HDA_DSP_REG_ADSPIC = 4, CONTROL = 8, TDR = 12,
       IDA = 16, TDD = 20, TDA = 24 };
#define HDA_DSP_IPC_INTERRUPT 1u
#define MTL_HFINTIPPTR_PTR_MASK 0xffu
#define MTL_DSP_IRQSTS 4u
#define MTL_DSP_IRQSTS_IPC 1u
#define SOF_FW_BOOT_FAILED 3
#define IO_NO_INCREMENT 0
typedef struct {
    unsigned IsAce, IntIpPtrReg, HipcTdr, HipcIda, HipcTdd, HipcTda, HipcCtl;
    unsigned HipcTdrBusy, HipcIdaDone, HipcCtlBusy, HipcCtlDone;
} HW;
typedef struct {
    const HW *HwDesc;
    ULONG *Bar4VirtualAddress;
    ULONG Bar4Length;
    void *WdfInterrupt;
    BOOLEAN IrqConnected, IrqArmed, IrqPending, IrqMaskNeeded;
    BOOLEAN IsMsi, Removed, IpcFaulted;
    int IpcLock, FwState, IpcReplyEvent, FwReadyEvent;
} CTX, *PSOF_DEVICE_CONTEXT;
typedef void *WDFINTERRUPT, *WDFDEVICE, *WDFOBJECT;
static CTX ctx;
static ULONG regs[7], reads, writes, queued, drained, handled;
static int locked, remove_during_dpc, fault_during_dpc, lost_control_write;
static int SofDeviceRemoved(CTX *c) { return c->Removed; }
static int pj_ipc4_mmio_span_valid(ULONG length, ULONG offset, ULONG bytes)
{
    return offset <= length && bytes <= length - offset;
}
static ULONG SofDspRead32(ULONG *base, ULONG offset)
{
    assert(!ctx.Removed && base == regs && offset / 4 < 7);
    ++reads;
    return regs[offset / 4];
}
static void SofDspWrite32(ULONG *base, ULONG offset, ULONG value)
{
    assert(locked && !ctx.Removed && base == regs && offset / 4 < 7);
    ++writes;
    if (!(lost_control_write && offset == CONTROL))
        regs[offset / 4] = value;
}
static void WdfInterruptAcquireLock(WDFINTERRUPT interrupt) { assert(interrupt == &ctx && !locked++); }
static void WdfInterruptReleaseLock(WDFINTERRUPT interrupt) { assert(interrupt == &ctx && --locked == 0); }
static void KeFlushQueuedDpcs(void) { assert(!locked && !ctx.IrqArmed && !ctx.IrqPending); ++drained; }
static CTX *GetDeviceContext(WDFDEVICE device) { assert(device == &ctx); return &ctx; }
static WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT interrupt) { return interrupt; }
static void WdfInterruptQueueDpcForIsr(WDFINTERRUPT interrupt) { assert(interrupt == &ctx && locked); ++queued; }
static void KeAcquireSpinLock(int *lock, KIRQL *irql) { assert(!locked && !(*lock)++); *irql = 0; }
static void KeReleaseSpinLock(int *lock, KIRQL irql) { assert(irql == 0 && --(*lock) == 0); }
static void KeSetEvent(int *event, int increment, int wait) { assert(!increment && !wait); ++*event; }
static BOOLEAN SofIpcDpcHandler(CTX *c)
{
    assert(!locked && c == &ctx && !(regs[CONTROL / 4] & 3));
    ++handled;
    regs[TDR / 4] = regs[IDA / 4] = 0;
    if (remove_during_dpc)
        c->Removed = TRUE;
    return !fault_during_dpc;
}
"""
cases = r"""
static BOOLEAN interrupt(ULONG message)
{
    BOOLEAN accepted;
    WdfInterruptAcquireLock(&ctx);
    accepted = SofEvtInterruptIsr(&ctx, message);
    WdfInterruptReleaseLock(&ctx);
    return accepted;
}

static void init(void)
{
    static const HW hw = {0, 0, TDR, IDA, TDD, TDA, CONTROL, 0x80000000u, 0x80000000u, 1, 2};
    ctx = (CTX){.HwDesc=&hw, .Bar4VirtualAddress=regs, .Bar4Length=sizeof(regs), .WdfInterrupt=&ctx};
    for (unsigned i = 0; i < 7; ++i) regs[i] = 0;
    reads = writes = queued = drained = handled = 0;
    remove_during_dpc = fault_during_dpc = lost_control_write = 0;
    WdfInterruptAcquireLock(&ctx);
    assert(SofEvtInterruptEnable(&ctx, &ctx) == STATUS_SUCCESS);
    WdfInterruptReleaseLock(&ctx);
}

int main(void)
{
    init();
    regs[IDA / 4] = 0x80000000u;
    assert(!interrupt(0) && !reads && !writes);
    assert(SofIpcEnableInterrupts(&ctx));
    assert(regs[HDA_DSP_REG_ADSPIC / 4] & HDA_DSP_IPC_INTERRUPT);
    assert((regs[CONTROL / 4] & 3) == 3);
    ULONG before = writes;
    assert(!interrupt(0) && writes == before);
    regs[HDA_DSP_REG_ADSPIS / 4] = HDA_DSP_IPC_INTERRUPT;
    ctx.IsMsi = TRUE;
    assert(!interrupt(1) && !queued);
    regs[IDA / 4] = MAXULONG;
    assert(!interrupt(0) && !queued);
    regs[IDA / 4] = 0x80000000u;
    regs[CONTROL / 4] = 1;
    assert(!interrupt(0) && !queued);
    regs[CONTROL / 4] = 3;
    assert(interrupt(0) && !(regs[CONTROL / 4] & 3) && queued == 1);
    assert(!interrupt(0) && queued == 1);
    SofEvtInterruptDpc(&ctx, NULL);
    assert(handled == 1 && (regs[CONTROL / 4] & 3) == 3);
    SofEvtInterruptDpc(&ctx, NULL);
    assert(handled == 1);

    ctx.IsMsi = FALSE;
    regs[TDR / 4] = 0x80000000u;
    assert(interrupt(0));
    assert(SofIpcDisableInterrupts(&ctx) && drained == 1);
    SofEvtInterruptDpc(&ctx, NULL);
    assert(handled == 1 && !regs[CONTROL / 4] && !regs[HDA_DSP_REG_ADSPIC / 4]);
    assert(!interrupt(0));

    init();
    lost_control_write = 1;
    assert(!SofIpcEnableInterrupts(&ctx) && !ctx.IrqArmed);
    assert(!regs[HDA_DSP_REG_ADSPIC / 4] && !interrupt(0));
    lost_control_write = 0;
    assert(SofIpcEnableInterrupts(&ctx));
    regs[HDA_DSP_REG_ADSPIS / 4] = HDA_DSP_IPC_INTERRUPT;
    regs[TDR / 4] = 0x80000000u;
    assert(interrupt(0));
    remove_during_dpc = 1;
    SofEvtInterruptDpc(&ctx, NULL);
    assert(!ctx.IrqArmed && !regs[CONTROL / 4] && ctx.IpcFaulted);
    assert(ctx.FwReadyEvent == 1 && ctx.IpcReplyEvent == 1 && !interrupt(0));
    assert(!SofIpcDisableInterrupts(&ctx) && drained == 1);

    init();
    assert(SofIpcEnableInterrupts(&ctx));
    WdfInterruptAcquireLock(&ctx);
    assert(SofEvtInterruptDisable(&ctx, &ctx) == STATUS_SUCCESS);
    WdfInterruptReleaseLock(&ctx);
    assert(!ctx.IrqConnected && !ctx.IrqArmed && !regs[CONTROL / 4] && !regs[HDA_DSP_REG_ADSPIC / 4]);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="pj_kmdf_irq_") as temp:
    src = Path(temp) / "test.c"
    exe = Path(temp) / ("test.exe" if os.name == "nt" else "test")
    src.write_text(fixture + control + isr + callbacks + cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)
print("PASS: 54C8 shared IRQ/MSI ownership, channel masking, DPC gating, and shutdown")
