/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Ipc.c
 * @brief Inter-Processor Communication (IPC) Protocol Handler for Sound Open Firmware.
 *
 * @details
 * Implements low-level register programming, shared SRAM mailbox data transfer,
 * doorbell signaling, interrupt mitigation, and IPC4 protocol dispatch between the
 * Windows host kernel and Intel cAVS / ACE Audio DSP microarchitectures.
 *
 * Hardware Mechanisms:
 *  1. Doorbell Registers:
 *     - Host Initiator: HIPCIDR (Primary Command + BUSY bit), HIPCIDD (Extension Data),
 *       HIPCIDA (DSP Acknowledgement + DONE bit).
 *     - DSP Target: HIPCTDR (Incoming Command/Notification + BUSY bit), HIPCTDD (Extension Data),
 *       HIPCTDA (Host Acknowledgement + DONE bit).
 *  2. Shared Mailbox Windows (DSP SRAM):
 *     - Downlink Mailbox (Host -> DSP): Used for transmitting parameter blocks, pipeline configs,
 *       and large data structures across BAR4.
 *     - Uplink Mailbox (DSP -> Host): Used for receiving response payloads, telemetry data, and
 *       firmware exception dumps.
 *  3. Interrupt Routing Hierarchy:
 *     - Top-Level IRQ: Interrupt IP Pointer register (MTL_HFINTIPPTR) points to top-level
 *       DSP IRQINTEN / IRQSTS registers.
 *     - Second-Level IRQ: HfHIPCIE (Host Interface IPC Interrupt Enable).
 *     - Channel-Level IRQ: HIPCCTL (Control register masking BUSY and DONE interrupt generation).
 *  4. Telemetry and Post-Mortem Debugging:
 *     - Captures DSP Core 0 Status (HFDSC), Core 0 Error (HFDEC), Extended ROM status registers,
 *       and parses Xtensa architecture exception frames (EXCCAUSE, EXCVADDR, EPC1, PS).
 *     - Generates binary (.bin) and structured text (.txt) coredumps directly to disk.
 * Copyright 2026 The SOF Authors
 */

#include "Device.h"
#include "Ipc.h"
#include "PujjoIpc4Protocol.h"
#include <ntstrsafe.h>

void SofLogBreadcrumb(const char* text);
NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(_In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock);

/**
 * @def SOF_IPC_MAX_PAYLOAD
 * @brief Maximum supported IPC payload buffer size in bytes (4096 = 4KB page).
 */
#define SOF_IPC_MAX_PAYLOAD 4096

/**
 * @brief Initializes IPC synchronization primitives and allocates the shared reply buffer.
 *
 * Prepares the host-side IPC infrastructure by initializing the spinlock protecting
 * transmit state, resetting communication events, allocating non-paged memory for
 * receiving DSP replies, and setting the firmware boot state to NOT_STARTED.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return STATUS_SUCCESS on success, or STATUS_INSUFFICIENT_RESOURCES if pool allocation fails.
 */
NTSTATUS
SofIpcInitialize(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    /* Initialize spinlock protecting IPC transmitter state and pending reply buffer */
    ExInitializeRundownProtection(&Ctx->IpcRundown);
    KeInitializeSpinLock(&Ctx->IpcLock);

    /* Notification events: manually cleared before dispatch, signaled by DPC on completion */
    KeInitializeEvent(&Ctx->IpcReplyEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Ctx->FwReadyEvent, NotificationEvent, FALSE);

    /* Allocate non-paged kernel pool for holding incoming mailbox reply payloads */
    Ctx->ReplyBufferSize = SOF_IPC_MAX_PAYLOAD;
    Ctx->ReplyBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       Ctx->ReplyBufferSize,
                                       SOF_DEVICE_TAG);
    if (!Ctx->ReplyBuffer) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Failed to allocate IPC reply buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Ctx->ReplyBuffer, Ctx->ReplyBufferSize);
    Ctx->IpcTxBusy = FALSE;
    Ctx->IpcReplyReady = FALSE;
    Ctx->IpcFaulted = FALSE;
    Ctx->IpcRequestPrimary = 0;
    RtlZeroMemory(&Ctx->PendingReply, sizeof(Ctx->PendingReply));
    Ctx->FwState = SOF_FW_BOOT_NOT_STARTED;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC subsystem initialized successfully.\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Releases IPC subsystem resources during driver unload or tear-down.
 *
 * Frees the allocated non-paged reply buffer and zeros out tracking fields.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcCleanup(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    KIRQL irql;
    if (!Ctx->ReplyBuffer) return;
    KeAcquireSpinLock(&Ctx->IpcLock, &irql);
    Ctx->IpcFaulted = TRUE;
    KeSetEvent(&Ctx->IpcReplyEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&Ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Ctx->IpcLock, irql);
    ExWaitForRundownProtectionRelease(&Ctx->IpcRundown);
    if (Ctx->ReplyBuffer) {
        ExFreePoolWithTag(Ctx->ReplyBuffer, SOF_DEVICE_TAG);
        Ctx->ReplyBuffer = NULL;
        Ctx->ReplyBufferSize = 0;
    }
}

/**
 * @brief Enables host-directed IPC interrupts in the DSP hardware registers.
 *
 * Programs the multi-level interrupt hierarchy on Intel hardware:
 *  1. Top-Level: Reads MTL_HFINTIPPTR to find the IRQINTEN register offset and enables HOST_IPC.
 *  2. Second-Level: Sets MTL_DSP_REG_HfHIPCIE_IE_MASK in HfHIPCIE.
 *  3. Channel-Level: Unmasks BUSY and DONE interrupts in HIPCCTL.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcEnableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u))
        return;

    if (Ctx->HwDesc->IsAce) {
        /*
         * ACE Interrupt Architecture Level 1:
         * Read Interrupt IP Pointer (MTL_HFINTIPPTR) to determine the base offset
         * of the top-level interrupt controller block within DSP BAR4 MMIO space.
         */
        ULONG rawPtr;
        ULONG hfintipptr;
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_HFINTIPPTR, 4u) ||
            !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_DSP_REG_HfHIPCIE, 4u))
            return;
        rawPtr = SofDspRead32(Ctx->Bar4VirtualAddress, MTL_HFINTIPPTR);
        if (rawPtr == 0xFFFFFFFFu)
            return;
        hfintipptr = rawPtr & MTL_HFINTIPPTR_PTR_MASK;
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, hfintipptr, 4u))
            return;

        /*
         * ACE Interrupt Architecture Level 1 Enable:
         * Unmask Host IPC interrupt generation in the primary IRQINTEN register.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         hfintipptr,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK);

        /*
         * ACE Interrupt Architecture Level 2 Enable:
         * Unmask the host-facing IPC interrupt in the intermediate HfHIPCIE register.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         MTL_DSP_REG_HfHIPCIE,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK);
    }

    /*
     * Channel-Level IPC Interrupt Enable (cAVS and ACE):
     * Program HIPCCTL to enable interrupt generation on:
     *  - BUSY: DSP raised an incoming message or notification in HIPCTDR.
     *  - DONE: DSP acknowledged host-transmitted command in HIPCIDA.
     */
    SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                     Ctx->HwDesc->HipcCtl,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone);

    if (!Ctx->HwDesc->IsAce && pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPIC, 4u))
        SofDspUpdateBits(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPIC,
                         HDA_DSP_IPC_INTERRUPT, HDA_DSP_IPC_INTERRUPT);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC interrupts enabled (IsAce=%d, CTL offset 0x%x).\n",
               Ctx->HwDesc->IsAce, Ctx->HwDesc->HipcCtl);
}

/**
 * @brief Disables host-directed IPC interrupts in the DSP hardware registers.
 *
 * Masks BUSY and DONE interrupts in HIPCCTL, and disables intermediate and top-level
 * interrupt registers on ACE platforms to guarantee quiet hardware before power transition.
 *
 * @param[in] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcDisableInterrupts(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u))
        return;

    if (!Ctx->HwDesc->IsAce && pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPIC, 4u))
        SofDspUpdateBits(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPIC,
                         HDA_DSP_IPC_INTERRUPT, 0);

    /*
     * Channel-Level IPC Interrupt Disable:
     * Mask both BUSY and DONE interrupts in the channel control register (HIPCCTL).
     */
    SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                     Ctx->HwDesc->HipcCtl,
                     Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                     0);

    if (Ctx->HwDesc->IsAce) {
        /* Read Interrupt IP Pointer to locate top-level IRQ block */
        ULONG rawPtr;
        ULONG hfintipptr;
        /* Keep channel interrupts masked even if the ACE hierarchy is invalid. */
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_HFINTIPPTR, 4u) ||
            !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, MTL_DSP_REG_HfHIPCIE, 4u))
            return;
        rawPtr = SofDspRead32(Ctx->Bar4VirtualAddress, MTL_HFINTIPPTR);
        if (rawPtr == 0xFFFFFFFFu)
            return;
        hfintipptr = rawPtr & MTL_HFINTIPPTR_PTR_MASK;
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, hfintipptr, 4u))
            return;

        /* Disable Host IPC interrupt in second-level HfHIPCIE */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         MTL_DSP_REG_HfHIPCIE,
                         MTL_DSP_REG_HfHIPCIE_IE_MASK,
                         0);

        /* Disable Host IPC in top-level IRQINTEN */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         hfintipptr,
                         MTL_IRQ_INTEN_L_HOST_IPC_MASK,
                         0);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC interrupts disabled.\n");
}

/**
 * @brief Captures and persists full DSP state, registers, mailboxes, and exception frames.
 *
 * Executed upon firmware crash notification (SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT), transmission
 * timeout, or fatal hardware error. Captures:
 *  - DSP Core 0 Status (HFDSC / 0x160200) and Error (HFDEC / 0x160204).
 *  - Extended ROM status registers [0..7].
 *  - All IPC doorbell registers (HIPCTDR, HIPCTDA, HIPCTDD, HIPCIDR, HIPCIDA, HIPCIDD, HIPCCTL).
 *  - Full 4KB Uplink and 4KB Downlink mailbox memory blocks.
 *  - Xtensa architecture exception frame (EXCCAUSE, EXCVADDR, EPC1, PS) if present.
 * Writes diagnostic files to:
 *  - C:\\sof_dsp_coredump.bin (raw binary for offline post-mortem analysis).
 *  - C:\\sof_dsp_coredump.txt (human-readable structured diagnostic report).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @param[in] Reason Diagnostic string identifying the failure trigger.
 */
VOID
SofDspDumpCore(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ const char *Reason
)
{
    LARGE_INTEGER sysTime;
    ULONG core0Status;
    ULONG core0Error;
    ULONG extRom[8] = { 0 };
    ULONG hipcTdr, hipcTda, hipcTdd, hipcIdr, hipcIda, hipcIdd, hipcCtl;
    ULONG dumpSize;
    PUCHAR dumpBuffer;
    PSOF_DSP_COREDUMP_HEADER hdr;
    PUCHAR uplinkDst;
    PUCHAR downlinkDst;
    PULONG upDwords;
    BOOLEAN hasOops = FALSE;
    ULONG exccause = 0, excvaddr = 0, epc1 = 0, ps = 0;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || KeAreAllApcsDisabled())
        return;
    /* Never read unmapped register or mailbox ranges in a crash handler. */
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomStatusReg, 32u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomErrorReg, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdd, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIdd, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u) ||
        Ctx->HwDesc->MailboxUplinkSize < 4096u ||
        Ctx->HwDesc->MailboxDownlinkSize < 4096u ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->MailboxUplinkOffset, 4096u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->MailboxDownlinkOffset, 4096u))
        return;

    KeQuerySystemTime(&sysTime);

    /* Read Core 0 hardware status and error registers from DSP SHIM */
    core0Status = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg);
    core0Error = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomErrorReg);

    /* Read extended ROM status array (8 consecutive DWORDs) */
    for (ULONG i = 0; i < 8; i++) {
        extRom[i] = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg + i * 4);
    }

    /* Read all IPC doorbell and control registers */
    hipcTdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    hipcTda = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda);
    hipcTdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdd);
    hipcIdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdr);
    hipcIda = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);
    hipcIdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdd);
    hipcCtl = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcCtl);

    /* Calculate total dump buffer size: Header + 4KB Uplink Mailbox + 4KB Downlink Mailbox + 64KB Debug Window */
    ULONG debugWinOffset = Ctx->DebugWindowOffset;
    ULONG debugWinSize = 0;
    if (debugWinOffset != 0 &&
        pj_ipc4_mmio_span_valid(Ctx->Bar4Length, debugWinOffset, 65536u))
        debugWinSize = 65536u;
    else
        debugWinOffset = 0;

    dumpSize = sizeof(SOF_DSP_COREDUMP_HEADER) + 4096 + 4096 + debugWinSize;
    dumpBuffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, dumpSize, SOF_DEVICE_TAG);
    if (!dumpBuffer) {
        SofLogBreadcrumb("[DSP-PANIC] Failed to allocate coredump buffer\n");
        return;
    }

    /* Populate structured core dump header */
    RtlZeroMemory(dumpBuffer, dumpSize);
    hdr = (PSOF_DSP_COREDUMP_HEADER)dumpBuffer;
    hdr->Magic = SOF_COREDUMP_MAGIC;
    hdr->Version = SOF_COREDUMP_VERSION;
    hdr->DumpSize = dumpSize;
    hdr->TimestampLow = sysTime.LowPart;
    hdr->TimestampHigh = sysTime.HighPart;
    hdr->VendorId = Ctx->VendorId;
    hdr->DeviceId = Ctx->DeviceId;
    if (Reason) {
        RtlStringCbCopyA(hdr->ReasonString, sizeof(hdr->ReasonString), Reason);
    }
    hdr->Core0Status = core0Status;
    hdr->Core0Error = core0Error;
    for (ULONG i = 0; i < 8; i++) {
        hdr->ExtRomStatus[i] = extRom[i];
    }
    hdr->HipcTdr = hipcTdr;
    hdr->HipcTda = hipcTda;
    hdr->HipcTdd = hipcTdd;
    hdr->HipcIdr = hipcIdr;
    hdr->HipcIda = hipcIda;
    hdr->HipcIdd = hipcIdd;
    hdr->HipcCtl = hipcCtl;
    hdr->UplinkMailboxOffset = Ctx->HwDesc->MailboxUplinkOffset;
    hdr->UplinkMailboxSize = 4096;
    hdr->DownlinkMailboxOffset = Ctx->HwDesc->MailboxDownlinkOffset;
    hdr->DownlinkMailboxSize = 4096;
    hdr->DebugWindowOffset = debugWinOffset;
    hdr->DebugWindowSize = debugWinSize;

    uplinkDst = dumpBuffer + sizeof(SOF_DSP_COREDUMP_HEADER);
    downlinkDst = uplinkDst + 4096;
    PUCHAR debugWinDst = downlinkDst + 4096;

    /*
     * Read full 4KB Uplink Mailbox (DSP SRAM) via 32-bit MMIO reads.
     * Contains firmware crash log, exception headers, and active pipeline context.
     */
    if (Ctx->HwDesc->MailboxUplinkOffset > 0) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxUplinkOffset);
        ULONG *dst = (ULONG *)uplinkDst;
        for (ULONG i = 0; i < 4096 / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Read full 4KB Downlink Mailbox (DSP SRAM) via 32-bit MMIO reads.
     * Contains the last command payload dispatched from host to DSP.
     */
    if (Ctx->HwDesc->MailboxDownlinkOffset > 0) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + Ctx->HwDesc->MailboxDownlinkOffset);
        ULONG *dst = (ULONG *)downlinkDst;
        for (ULONG i = 0; i < 4096 / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Read 64KB SRAM Debug Window (Page 0 descriptors + Core slots) via 32-bit MMIO reads.
     * Captures Zephyr Mtrace circular buffers for offline post-mortem log reconstruction.
     */
    if (debugWinSize != 0u) {
        volatile ULONG *src = (volatile ULONG *)((PUCHAR)Ctx->Bar4VirtualAddress + debugWinOffset);
        ULONG *dst = (ULONG *)debugWinDst;
        for (ULONG i = 0; i < debugWinSize / sizeof(ULONG); i++) {
            dst[i] = src[i];
        }
    }

    /*
     * Parse Xtensa Architecture Exception / Oops Frame:
     * If the Uplink mailbox begins with type=1 and stack frame size >= 28,
     * extract hardware exception registers: EXCCAUSE, EXCVADDR, PS, EPC1.
     */
    upDwords = (PULONG)uplinkDst;
    if (upDwords[0] == 1 && upDwords[1] >= 28) {
        hasOops = TRUE;
        exccause = upDwords[7];
        excvaddr = upDwords[8];
        ps = upDwords[9];
        epc1 = upDwords[10];
    }

    /* Format Breadcrumb / Debugger Output */
    {
        char msg[256];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] *** DSP EXCEPTION DUMP: Reason=%s ***\n",
            Reason ? Reason : "UNKNOWN");
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] Core0Status=0x%08X Core0Error=0x%08X TDR=0x%08X IDR=0x%08X\n",
            core0Status, core0Error, hipcTdr, hipcIdr);
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[DSP-PANIC] ExtROM: [0..3]=0x%08X 0x%08X 0x%08X 0x%08X [4..7]=0x%08X 0x%08X 0x%08X 0x%08X\n",
            extRom[0], extRom[1], extRom[2], extRom[3],
            extRom[4], extRom[5], extRom[6], extRom[7]);
        SofLogBreadcrumb(msg);
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);

        if (hasOops) {
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[DSP-PANIC] Xtensa Oops: EXCCAUSE=0x%X EXCVADDR=0x%08X EPC1=0x%08X PS=0x%08X\n",
                exccause, excvaddr, epc1, ps);
            SofLogBreadcrumb(msg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", msg);
        }
    }

    /*
     * Persist Raw Binary Dump File:
     * Written to \DosDevices\C:\sof_dsp_coredump.bin for deep analysis by tools like coredumper.
     */
    {
        UNICODE_STRING binPath;
        OBJECT_ATTRIBUTES binOa;
        IO_STATUS_BLOCK binIo;
        HANDLE hBin = NULL;

        RtlInitUnicodeString(&binPath, L"\\DosDevices\\C:\\sof_dsp_coredump.bin");
        InitializeObjectAttributes(&binOa, &binPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        if (NT_SUCCESS(ZwCreateFile(&hBin, GENERIC_WRITE | SYNCHRONIZE, &binOa, &binIo, NULL,
                                     FILE_ATTRIBUTE_NORMAL,
                                     FILE_SHARE_READ,
                                     FILE_OVERWRITE_IF,
                                     FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            ZwWriteFile(hBin, NULL, NULL, NULL, &binIo, dumpBuffer, dumpSize, NULL, NULL);
            ZwFlushBuffersFile(hBin, &binIo);
            ZwClose(hBin);
            SofLogBreadcrumb("[DSP-PANIC] Binary core dump written to C:\\sof_dsp_coredump.bin\n");
        } else {
            SofLogBreadcrumb("[DSP-PANIC] Failed to write C:\\sof_dsp_coredump.bin\n");
        }
    }

    /*
     * Persist Human-Readable Text Report:
     * Written to \DosDevices\C:\sof_dsp_coredump.txt for instant field diagnostics without symbol setup.
     */
    {
        UNICODE_STRING txtPath;
        OBJECT_ATTRIBUTES txtOa;
        IO_STATUS_BLOCK txtIo;
        HANDLE hTxt = NULL;

        RtlInitUnicodeString(&txtPath, L"\\DosDevices\\C:\\sof_dsp_coredump.txt");
        InitializeObjectAttributes(&txtOa, &txtPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        if (NT_SUCCESS(ZwCreateFile(&hTxt, GENERIC_WRITE | SYNCHRONIZE, &txtOa, &txtIo, NULL,
                                     FILE_ATTRIBUTE_NORMAL,
                                     FILE_SHARE_READ,
                                     FILE_OVERWRITE_IF,
                                     FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            char txtBuf[2048];
            PULONG downDwords = (PULONG)downlinkDst;
            RtlStringCbPrintfA(txtBuf, sizeof(txtBuf),
                "================================================================================\r\n"
                "SOUND OPEN FIRMWARE (SOF) DSP EXCEPTION / CORE DUMP REPORT\r\n"
                "================================================================================\r\n"
                "Reason: %s\r\n"
                "Timestamp: 0x%08X%08X\r\n"
                "Hardware: Vendor=0x%04X, Device=0x%04X\r\n"
                "\r\n"
                "DSP Core0 Status (HFDSC @ 0x160200): 0x%08X\r\n"
                "DSP Core0 Error  (HFDEC @ 0x160204): 0x%08X\r\n"
                "Extended ROM Status [0..7]:\r\n"
                "  [0]: 0x%08X  [1]: 0x%08X  [2]: 0x%08X  [3]: 0x%08X\r\n"
                "  [4]: 0x%08X  [5]: 0x%08X  [6]: 0x%08X  [7]: 0x%08X\r\n"
                "\r\n"
                "IPC Channel Registers:\r\n"
                "  HIPCTDR: 0x%08X (Busy=%u)\r\n"
                "  HIPCTDA: 0x%08X (Done=%u)\r\n"
                "  HIPCTDD: 0x%08X\r\n"
                "  HIPCIDR: 0x%08X (Busy=%u)\r\n"
                "  HIPCIDA: 0x%08X (Done=%u)\r\n"
                "  HIPCIDD: 0x%08X\r\n"
                "  HIPCCTL: 0x%08X\r\n"
                "\r\n"
                "Xtensa Oops / Exception Frame Analysis:\r\n"
                "  Oops Detected: %s\r\n"
                "  EXCCAUSE:      0x%08X\r\n"
                "  EXCVADDR:      0x%08X\r\n"
                "  EPC1:          0x%08X\r\n"
                "  PS:            0x%08X\r\n"
                "\r\n"
                "Uplink Mailbox First 32 Bytes (Hex):\r\n"
                "  %08X %08X %08X %08X %08X %08X %08X %08X\r\n"
                "Downlink Mailbox First 32 Bytes (Hex):\r\n"
                "  %08X %08X %08X %08X %08X %08X %08X %08X\r\n"
                "================================================================================\r\n",
                Reason ? Reason : "UNKNOWN",
                hdr->TimestampHigh, hdr->TimestampLow,
                hdr->VendorId, hdr->DeviceId,
                core0Status, core0Error,
                extRom[0], extRom[1], extRom[2], extRom[3],
                extRom[4], extRom[5], extRom[6], extRom[7],
                hipcTdr, (hipcTdr & Ctx->HwDesc->HipcTdrBusy) ? 1 : 0,
                hipcTda, (hipcTda & Ctx->HwDesc->HipcTdaDone) ? 1 : 0,
                hipcTdd,
                hipcIdr, (hipcIdr & Ctx->HwDesc->HipcIdrBusy) ? 1 : 0,
                hipcIda, (hipcIda & Ctx->HwDesc->HipcIdaDone) ? 1 : 0,
                hipcIdd, hipcCtl,
                hasOops ? "YES" : "NO",
                exccause, excvaddr, epc1, ps,
                upDwords[0], upDwords[1], upDwords[2], upDwords[3],
                upDwords[4], upDwords[5], upDwords[6], upDwords[7],
                downDwords[0], downDwords[1], downDwords[2], downDwords[3],
                downDwords[4], downDwords[5], downDwords[6], downDwords[7]);

            size_t txtLen = 0;
            RtlStringCbLengthA(txtBuf, sizeof(txtBuf), &txtLen);
            ZwWriteFile(hTxt, NULL, NULL, NULL, &txtIo, txtBuf, (ULONG)txtLen, NULL, NULL);
            ZwFlushBuffersFile(hTxt, &txtIo);
            ZwClose(hTxt);
            SofLogBreadcrumb("[DSP-PANIC] Formatted text report written to C:\\sof_dsp_coredump.txt\n");
        } else {
            SofLogBreadcrumb("[DSP-PANIC] Failed to write C:\\sof_dsp_coredump.txt\n");
        }
    }

    ExFreePoolWithTag(dumpBuffer, SOF_DEVICE_TAG);
}

/**
 * @brief Fast Interrupt Service Routine (ISR) check for pending IPC hardware events.
 *
 * Checks top-level and channel-level doorbell registers:
 *  - On ACE, verifies top-level IRQ status (MTL_DSP_IRQSTS_IPC).
 *  - Checks if DSP raised an incoming message (HIPCTDR BUSY asserted).
 *  - Checks if DSP completed a host command (HIPCIDA DONE asserted).
 * If either condition is met, masks HIPCCTL BUSY and DONE interrupts immediately to
 * eliminate line-based IRQ storms, then returns TRUE to schedule the DPC.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return TRUE if an IPC interrupt was acknowledged and DPC is needed; FALSE otherwise.
 */
BOOLEAN
SofIpcIsrCheck(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    ULONG tdr;
    ULONG ida;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return FALSE;
    /* The initial ROM ACK is exclusively consumed by the passive loader. */
    if (Ctx->FwState == SOF_FW_BOOT_NOT_STARTED)
        return FALSE;

    /* Shared IRQs must never probe outside the mapped BAR. A malformed
     * descriptor fails closed without acknowledging another device's IRQ. */
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdd, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u))
        return FALSE;

    /* For ACE platforms (MTL, LNL, PTL), verify top-level DSP IRQ status register */
    if (Ctx->HwDesc->IsAce && Ctx->HwDesc->IntIpPtrReg) {
        ULONG ptr;
        ULONG irqSts;
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length,
                                    Ctx->HwDesc->IntIpPtrReg, 4u))
            return FALSE;
        ptr = SofDspRead32(Ctx->Bar4VirtualAddress,
                           Ctx->HwDesc->IntIpPtrReg);
        if (ptr == 0xFFFFFFFFu)
            return FALSE;
        ptr &= MTL_HFINTIPPTR_PTR_MASK;
        /* Check before addition so even hostile firmware offsets cannot wrap. */
        if (ptr > Ctx->Bar4Length ||
            MTL_DSP_IRQSTS > Ctx->Bar4Length - ptr ||
            !pj_ipc4_mmio_span_valid(Ctx->Bar4Length,
                                    ptr + MTL_DSP_IRQSTS, 4u))
            return FALSE;
        irqSts = SofDspRead32(Ctx->Bar4VirtualAddress, ptr + MTL_DSP_IRQSTS);

        /* Return FALSE if register is unmapped or IPC bit is not asserted. */
        if (irqSts == 0xFFFFFFFF || !(irqSts & MTL_DSP_IRQSTS_IPC))
            return FALSE;
    } else if (!Ctx->HwDesc->IsAce) {
        ULONG irqSts;
        if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, HDA_DSP_REG_ADSPIS, 4u))
            return FALSE;
        irqSts = SofDspRead32(Ctx->Bar4VirtualAddress, HDA_DSP_REG_ADSPIS);
        if (irqSts == MAXULONG || !(irqSts & HDA_DSP_IPC_INTERRUPT))
            return FALSE;
    }

    /* Check whether DSP raised an incoming message (TDR BUSY) or acknowledged our command (IDA DONE) */
    tdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    ida = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);
    /* All-ones doorbell reads can mean an unavailable PCI device, not an IRQ. */
    if (tdr == 0xFFFFFFFFu || ida == 0xFFFFFFFFu)
        return FALSE;

    if ((tdr & Ctx->HwDesc->HipcTdrBusy) || (ida & Ctx->HwDesc->HipcIdaDone)) {
        /*
         * CRITICAL: Mask IPC interrupts immediately in ISR to avoid an interrupt storm
         * on shared line-based interrupts before the DPC can run.
         */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy | Ctx->HwDesc->HipcCtlDone,
                         0);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Deferred Procedure Call (DPC) worker for dispatching IPC replies and notifications.
 *
 * Evaluates pending hardware status:
 *  1. Host TX ACK: DSP set DONE in HIPCIDA. Acknowledges DONE in HIPCIDA, re-enables HIPCCTL.
 *  2. DSP Incoming Message: DSP set BUSY in HIPCTDR.
 *     - If message direction bit (SOF_IPC4_MSG_DIR_MASK) is 1, this is a command reply.
 *       Reads payload from Uplink Mailbox, unlocks transmitter, and signals IpcReplyEvent.
 *     - If message direction bit is 0, this is an unsolicited notification.
 *       Handles SOF_IPC4_NOTIFY_FW_READY (signals FwReadyEvent) or
 *       SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT (triggers SofDspDumpCore).
 *  3. Doorbell Acknowledgment: Clears BUSY in HIPCTDR and HIPCTDA to acknowledge receipt to DSP.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 */
VOID
SofIpcDpcHandler(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    ULONG ida;
    ULONG tdr;
    BOOLEAN signalReply = FALSE;
    BOOLEAN bootReady = FALSE;
    BOOLEAN bootFault = FALSE;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc)
        return;
    /* A stale queued DPC cannot consume the initial ROM ACK. */
    if (Ctx->FwState == SOF_FW_BOOT_NOT_STARTED)
        return;
    /* Bounds check register windows actually used by this handler. */
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTdd, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcTda, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcCtl, 4u))
        return;

    /*
     * 1. Check if DSP completed our previous host-initiated request (ACK / DONE in IDA)
     */
    ida = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIda);
    tdr = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr);
    if (tdr == 0xFFFFFFFFu || ida == 0xFFFFFFFFu)
        return;
    if (ida & Ctx->HwDesc->HipcIdaDone) {
        /* Mask DONE in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlDone,
                         0);

        if (Ctx->HwDesc->IsAce) {
            /* Set DONE bit in IDA to acknowledge receipt to DSP on ACE */
            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcIda,
                             Ctx->HwDesc->HipcIdaDone,
                             Ctx->HwDesc->HipcIdaDone);
        } else {
            /* Write DONE bit to IDA on cAVS */
            SofDspWrite32(Ctx->Bar4VirtualAddress,
                          Ctx->HwDesc->HipcIda,
                          Ctx->HwDesc->HipcIdaDone);
        }

        /* Unmask DONE in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlDone,
                         Ctx->HwDesc->HipcCtlDone);

        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] IPC Host TX ACK received from DSP (IDA=0x%08x).\n",
                   ida);
    }

    /*
     * 2. Check if DSP sent an incoming message (Reply or Notification in TDR)
     */
    /* Use the checked snapshot above; new pending messages can raise another IRQ. */
    if (tdr & Ctx->HwDesc->HipcTdrBusy) {
        ULONG tdd = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdd);
        ULONG primary = tdr & 0x7FFFFFFF;
        ULONG extension = tdd & 0x7FFFFFFF;

        /* Is this a reply to our request or an unsolicited notification? */
        if (primary & SOF_IPC4_MSG_DIR_MASK) {
            /*
             * Reply Message: Matches outstanding host request.
             */
            KIRQL oldIrql;
            KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);

            /* IPC4 has no universal wire transaction ID. Never retry a
             * timed-out session without a separately verified hardware reset. */
            if (Ctx->IpcTxBusy && !Ctx->IpcReplyReady && !Ctx->IpcFaulted &&
                pj_ipc4_reply_matches(Ctx->IpcRequestPrimary, primary)) {
                Ctx->PendingReply.Primary = primary;
                Ctx->PendingReply.Extension = extension;
                Ctx->PendingReply.Payload = NULL;
                Ctx->PendingReply.PayloadSize = 0;
                if (tdd == MAXULONG) {
                    Ctx->IpcFaulted = TRUE;
                } else if (Ctx->IpcReplyCapacity && !(primary & SOF_IPC4_REPLY_STATUS)) {
                    PJ_DWORD bytes = 0;
                    if (pj_ipc4_large_reply_size(Ctx->IpcRequestExtension, tdd,
                            Ctx->IpcReplyCapacity, &bytes) != PJ_IPC4_VALID) {
                        Ctx->IpcFaulted = TRUE;
                    } else {
                        /* The sender validated the entire read window before
                         * ringing the doorbell. Copy only the declared bytes. */
                        PUCHAR mailbox = (PUCHAR)Ctx->Bar4VirtualAddress +
                                         Ctx->HwDesc->MailboxUplinkOffset;
                        PUCHAR buffer = (PUCHAR)Ctx->ReplyBuffer;
                        for (ULONG i = 0; i < bytes; i += sizeof(ULONG)) {
                            ULONG word = READ_REGISTER_ULONG((PULONG)(mailbox + i));
                            for (ULONG n = 0; n < sizeof(ULONG) && i + n < bytes; ++n)
                                buffer[i + n] = (UCHAR)(word >> (n * 8u));
                        }
                        Ctx->PendingReply.PayloadSize = bytes;
                    }
                }
                Ctx->IpcReplyReady = !Ctx->IpcFaulted;
                signalReply = TRUE;
            }
            KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] IPC Reply: Primary 0x%08x, Ext 0x%08x (status=0x%x)\n",
                       primary, extension, primary & SOF_IPC4_REPLY_STATUS);
        } else {
            /*
             * Unsolicited Notification: Sent asynchronously by DSP firmware.
             */
            ULONG notifyType = SOF_IPC4_NOTIFICATION_TYPE_GET(primary);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] IPC Notification: Type %u (0x%08x|0x%08x)\n",
                       notifyType, primary, extension);

            if (pj_ipc4_is_fw_ready(primary)) {
                /* Publish readiness only after acknowledging the notification. */
                bootReady = TRUE;
            } else if (pj_ipc4_is_notification(primary) &&
                       notifyType == SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT) {
                /* Firmware panic/exception caught by base FW */
                char bmsg[128];
                RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                    "[DSP-PANIC] *** FIRMWARE EXCEPTION NOTIFICATION RECEIVED (0x%08x|0x%08x) ***\n",
                    primary, extension);
                SofLogBreadcrumb(bmsg);
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", bmsg);
                /* File dumps are not legal at DISPATCH_LEVEL. */
                {
                    KIRQL oldIrql;
                    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
                    Ctx->IpcFaulted = TRUE;
                    Ctx->FwState = SOF_FW_BOOT_FAILED;
                    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
                    signalReply = TRUE;
                    bootFault = TRUE;
                }
            } else if (pj_ipc4_is_notification(primary) &&
                       notifyType == SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS) {
                /*
                 * Firmware Log Buffer Status: DSP has generated new log data
                 * in its SRAM debug slot (CONFIG_LOG_BACKEND_ADSP_MTRACE).
                 */
                ULONG core = SOF_IPC4_LOG_CORE_GET(primary);
                /* Defer SRAM log draining pending bounds/ownership review. */
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                           "[SOF] Firmware log notification for core %u (drain disabled).\n", core);
            }
        }

        /*
         * 3. Acknowledge message receipt to DSP:
         * On ACE (mtl_ipc_host_done):
         *  - Step a: Set busy bit in TDR to clear the interrupt assertion.
         *  - Step b: Clear busy bit in TDA to signal completion back to DSP.
         * On cAVS:
         *  - Write busy bit to TDR and done bit to TDA.
         */
        if (Ctx->HwDesc->IsAce) {
            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcTdr,
                             Ctx->HwDesc->HipcTdrBusy,
                             Ctx->HwDesc->HipcTdrBusy);

            SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                             Ctx->HwDesc->HipcTda,
                             Ctx->HwDesc->HipcTdaDone,
                             0);
        } else {
            SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTdr, Ctx->HwDesc->HipcTdrBusy);
            SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda, Ctx->HwDesc->HipcTdaDone);
        }

        /* Unmask Busy interrupt in HipcCtl */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy,
                         Ctx->HwDesc->HipcCtlBusy);
    } else {
        /* Ensure BUSY interrupt remains unmasked */
        SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                         Ctx->HwDesc->HipcCtl,
                         Ctx->HwDesc->HipcCtlBusy,
                         Ctx->HwDesc->HipcCtlBusy);
    }
    /* The ISR masks both sources. Restore DONE even for a BUSY-only IRQ. */
    SofDspUpdateBits(Ctx->Bar4VirtualAddress,
                     Ctx->HwDesc->HipcCtl,
                     Ctx->HwDesc->HipcCtlDone,
                     Ctx->HwDesc->HipcCtlDone);

    /* Acknowledge device notification before publishing boot/reply completion. */
    if (bootReady || bootFault) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        if (bootReady && !Ctx->IpcFaulted &&
            Ctx->FwState == SOF_FW_BOOT_IN_PROGRESS) {
            Ctx->FwState = SOF_FW_BOOT_READY_RECEIVED;
            KeSetEvent(&Ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
        } else if (bootFault) {
            /* Wake the loader promptly; the fault state is already latched. */
            KeSetEvent(&Ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
    }
    if (signalReply) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
        if (Ctx->IpcTxBusy && (Ctx->IpcReplyReady || Ctx->IpcFaulted))
            KeSetEvent(&Ctx->IpcReplyEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
    }
}

/**
 * @brief Synchronously transmits an IPC4 message and awaits the DSP reply.
 *
 * Transmit Sequence:
 *  1. Acquires IpcLock spinlock and checks transmitter busy state.
 *  2. Writes optional payload data into the Downlink Mailbox (DSP SRAM) using 32-bit MMIO writes.
 *  3. Writes Extension header to HIPCIDD.
 *  4. Rings the Doorbell: Writes Primary header with BUSY bit asserted to HIPCIDR.
 *  5. Releases spinlock and enters a wait loop on IpcReplyEvent.
 *  6. Polling Fallback: Checks every 25ms and manually calls SofIpcDpcHandler in case a line-based
 *     interrupt was delayed or lost under heavy system load.
 *  7. On completion, validates the IPC4 reply status code (bits 0..23). On timeout, triggers SofDspDumpCore.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @param[in]     Msg Pointer to the IPC message struct to send.
 * @param[out]    Reply Optional pointer to receive reply headers and payload.
 * @param[in]     TimeoutMs Maximum duration to await response in milliseconds.
 * @return STATUS_SUCCESS on success; STATUS_DEVICE_BUSY if already sending; STATUS_IO_TIMEOUT
 *         on timeout; STATUS_UNSUCCESSFUL if DSP returned an error code.
 */
static NTSTATUS
SofIpcSendWhilePowered(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_IPC_MESSAGE Msg,
    _Inout_opt_ PSOF_IPC_MESSAGE Reply,
    _In_ ULONG TimeoutMs,
    _In_ SOF_FW_BOOT_STATE RequiredState
)
{
    KIRQL oldIrql;
    LARGE_INTEGER timeout;
    NTSTATUS status = STATUS_IO_TIMEOUT;
    ULONG replyPrimary;
    ULONG replyExtension;
    ULONG hostDoorbell;
    ULONG capacity = Reply ? Reply->PayloadSize : 0;

    if (!Ctx || !Msg || TimeoutMs == 0 || TimeoutMs > 60000)
        return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Ctx->Bar4VirtualAddress || !Ctx->HwDesc || !Ctx->ReplyBuffer)
        return STATUS_DEVICE_NOT_READY;
    if ((Msg->Primary & PJ_IPC4_RESERVED_MASK) != 0u ||
        pj_ipc4_is_reply(Msg->Primary) || pj_ipc4_is_notification(Msg->Primary))
        return STATUS_INVALID_PARAMETER;
    /* This transport implements single-block LARGE_CONFIG_GET payloads only. */
    if (Reply && ((capacity != 0) != (Reply->Payload != NULL)))
        return STATUS_INVALID_PARAMETER;
    if (capacity) {
        PJ_DWORD ignored;
        if ((Msg->Primary & PJ_IPC4_CORRELATION_MASK) != PJ_IPC4_MODULE_LARGE_GET ||
            Msg->PayloadSize || capacity > Ctx->ReplyBufferSize ||
            pj_ipc4_large_reply_size(Msg->Extension, Msg->Extension | (1u << 28),
                capacity, &ignored) != PJ_IPC4_VALID)
            return STATUS_INVALID_PARAMETER;
        if (((capacity + 3u) & ~3u) > Ctx->HwDesc->MailboxUplinkSize ||
            !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->MailboxUplinkOffset,
                (capacity + 3u) & ~3u))
            return STATUS_INVALID_BUFFER_SIZE;
    }
    if (!pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIdr, 4u) ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->HipcIdd, 4u))
        return STATUS_INVALID_BUFFER_SIZE;
    /* Do not silently truncate mailbox payloads or read past their source.
     * Round up only when the mapped mailbox can hold the padded DWORD. */
    if ((Msg->PayloadSize != 0 && !Msg->Payload) ||
        Msg->PayloadSize > SOF_IPC_MAX_PAYLOAD ||
        Msg->PayloadSize > Ctx->HwDesc->MailboxDownlinkSize ||
        ((Msg->PayloadSize + 3u) & ~3u) > Ctx->HwDesc->MailboxDownlinkSize)
        return STATUS_INVALID_BUFFER_SIZE;
    if (Msg->PayloadSize && !pj_ipc4_mmio_span_valid(
            Ctx->Bar4Length, Ctx->HwDesc->MailboxDownlinkOffset,
            (Msg->PayloadSize + 3u) & ~3u))
        return STATUS_INVALID_BUFFER_SIZE;

    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);

    if (Ctx->IpcFaulted || Ctx->FwState != RequiredState) {
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_NOT_READY;
    }
    if (Ctx->IpcTxBusy) {
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }

    /* Software ownership alone is insufficient: don't overwrite a command
     * that the DSP has not consumed. An all-ones read indicates inaccessible
     * MMIO, not a legitimate BUSY state. Check under the transmitter lock. */
    hostDoorbell = SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdr);
    if (hostDoorbell == 0xFFFFFFFFu) {
        Ctx->IpcFaulted = TRUE;
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_NOT_READY;
    }
    if (hostDoorbell & Ctx->HwDesc->HipcIdrBusy) {
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }

    Ctx->IpcTxBusy = TRUE;
    Ctx->IpcReplyReady = FALSE;
    Ctx->IpcRequestPrimary = Msg->Primary;
    Ctx->IpcRequestExtension = Msg->Extension;
    Ctx->IpcReplyCapacity = capacity;
    RtlZeroMemory(&Ctx->PendingReply, sizeof(Ctx->PendingReply));
    KeResetEvent(&Ctx->IpcReplyEvent);

    /*
     * Write payload to Mailbox Downlink if present using 32-bit DWORD MMIO writes.
     * Host writes must align to DWORD boundaries for DSP SRAM controller consistency.
     */
    if (Msg->Payload && Msg->PayloadSize > 0) {
        ULONG offset;
        const UCHAR *src = (const UCHAR *)Msg->Payload;
        PUCHAR mailbox = (PUCHAR)Ctx->Bar4VirtualAddress +
                         Ctx->HwDesc->MailboxDownlinkOffset;
        for (offset = 0; offset < Msg->PayloadSize; offset += sizeof(ULONG)) {
            ULONG word = 0;
            ULONG byteIndex;
            for (byteIndex = 0; byteIndex < sizeof(ULONG) &&
                 offset + byteIndex < Msg->PayloadSize; ++byteIndex)
                word |= ((ULONG)src[offset + byteIndex]) << (byteIndex * 8u);
            WRITE_REGISTER_ULONG((PULONG)(mailbox + offset), word);
        }
    }

    /* Write extension header to HIPCIDD */
    SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcIdd, Msg->Extension);

    /*
     * Ring Doorbell:
     * Write Primary header with BUSY bit asserted to HIPCIDR.
     * This signals the DSP hardware scheduler that a host command is ready for processing.
     */
    SofDspWrite32(Ctx->Bar4VirtualAddress,
                  Ctx->HwDesc->HipcIdr,
                  Msg->Primary | Ctx->HwDesc->HipcIdrBusy);

    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

    {
        char bmsg[128];
        RtlStringCbPrintfA(bmsg, sizeof(bmsg),
            "[IPC-TX] Pri=0x%08x Ext=0x%08x Sz=%u\n",
            Msg->Primary, Msg->Extension, Msg->PayloadSize);
        SofLogBreadcrumb(bmsg);
    }
    /* Do not log unbounded payload DWORDs or read back beyond the mailbox.
     * The preceding IPC-TX breadcrumb already records length and headers. */

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] IPC TX Sent: Primary 0x%08x, Ext 0x%08x\n",
               Msg->Primary, Msg->Extension);

    /* Wait at passive level; do not also call the interrupt handler here.
     * A timeout faults the session rather than permitting a late untagged
     * response to satisfy a subsequent command. */
    timeout.QuadPart = -10000LL * (LONGLONG)TimeoutMs;
    status = KeWaitForSingleObject(&Ctx->IpcReplyEvent, Executive, KernelMode,
                                   FALSE, &timeout);
    KeAcquireSpinLock(&Ctx->IpcLock, &oldIrql);
    if (status != STATUS_SUCCESS || !Ctx->IpcReplyReady || Ctx->IpcFaulted) {
        Ctx->IpcFaulted = TRUE;
        Ctx->IpcTxBusy = FALSE;
        Ctx->IpcReplyReady = FALSE;
        KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);
        return status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : STATUS_DEVICE_NOT_READY;
    }

    replyPrimary = Ctx->PendingReply.Primary;
    replyExtension = Ctx->PendingReply.Extension;
    if (Reply) {
        Reply->Primary = replyPrimary;
        Reply->Extension = replyExtension;
        Reply->PayloadSize = Ctx->PendingReply.PayloadSize;
        if (Reply->PayloadSize)
            RtlCopyMemory(Reply->Payload, Ctx->ReplyBuffer, Reply->PayloadSize);
    }
    Ctx->IpcReplyReady = FALSE;
    Ctx->IpcTxBusy = FALSE;
    KeReleaseSpinLock(&Ctx->IpcLock, oldIrql);

    if ((replyPrimary & SOF_IPC4_REPLY_STATUS) != 0u) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] IPC firmware error: 0x%08x (extension 0x%08x)\n",
                   replyPrimary, replyExtension);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
SofIpcSendMessage(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ PSOF_IPC_MESSAGE Msg,
    _Inout_opt_ PSOF_IPC_MESSAGE Reply,
    _In_ ULONG TimeoutMs
)
{
    NTSTATUS status;
    if (!Ctx || !Msg) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!ExAcquireRundownProtection(&Ctx->IpcRundown))
        return STATUS_DEVICE_NOT_READY;
    status = SofIpcSendWhilePowered(Ctx, Msg, Reply, TimeoutMs, SOF_FW_BOOT_COMPLETE);
    ExReleaseRundownProtection(&Ctx->IpcRundown);
    return status;
}

NTSTATUS SofIpcGetBootFirmwareVersion(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _Out_writes_(4) USHORT Version[4]
)
{
    pj_ipc4_packet packet;
    SOF_IPC_MESSAGE request = {0}, reply = {0};
    NTSTATUS status;

    if (!Ctx || !Version) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (pj_ipc4_large_config_get(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID,
            SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID, SOF_IPC4_FW_PARAM_FW_CONFIG,
            PJ_IPC4_MAX_MAILBOX_PAYLOAD, &packet) != PJ_IPC4_VALID)
        return STATUS_INVALID_PARAMETER;

    request.Primary = packet.primary;
    request.Extension = packet.extension;
    reply.PayloadSize = PJ_IPC4_MAX_MAILBOX_PAYLOAD;
    reply.Payload = ExAllocatePool2(POOL_FLAG_NON_PAGED, reply.PayloadSize, SOF_DEVICE_TAG);
    if (!reply.Payload) return STATUS_INSUFFICIENT_RESOURCES;

    status = STATUS_DEVICE_NOT_READY;
    if (ExAcquireRundownProtection(&Ctx->IpcRundown)) {
        status = SofIpcSendWhilePowered(Ctx, &request, &reply, 500,
                                        SOF_FW_BOOT_READY_RECEIVED);
        ExReleaseRundownProtection(&Ctx->IpcRundown);
    }
    if (NT_SUCCESS(status)) {
        pj_ipc4_boot_caps caps;
        /* One DMIC and one analog lease can coexist. Require the full
         * mailbox capacity used by the current single-block transport. */
        if (pj_ipc4_fw_boot_caps(reply.Payload, reply.PayloadSize,
            PJ_IPC4_MAX_MAILBOX_PAYLOAD, 2u, &caps) != PJ_IPC4_VALID) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            RtlCopyMemory(Version, caps.version, sizeof(caps.version));
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "[SOF] FW_CONFIG: DL=%lu UL=%lu pipelines=%lu\n",
                (ULONG)caps.dl_bytes, (ULONG)caps.ul_bytes, (ULONG)caps.max_pipelines);
        }
    }
    ExFreePoolWithTag(reply.Payload, SOF_DEVICE_TAG);
    return status;
}

/**
 * @brief Reads the 32-bit DSP Firmware Status register (FWSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit status value, or 0 if unmapped.
 */
ULONG
SofReadFwStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->FwStatusReg, 4u))
        return 0;

    return SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->FwStatusReg);
}

/**
 * @brief Reads the 32-bit DSP ROM Status register (ROMSTATUS / HFDSC).
 *
 * @param[in] Ctx Pointer to the SOF device context.
 * @return 32-bit status value, or 0 if unmapped.
 */
ULONG
SofReadRomStatus(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, Ctx->HwDesc->RomStatusReg, 4u))
        return 0;

    return SofDspRead32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->RomStatusReg);
}

/**
 * @brief Reads unread Zephyr firmware logs from the DSP SRAM Debug Window.
 *
 * Implements the host side of the Zephyr ADSP Mtrace protocol (CONFIG_LOG_BACKEND_ADSP_MTRACE).
 * Invoked on SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS notification from DSP.
 *
 * Steps:
 *  1. Identifies debug slot offset for the given DSP core from Page 0 descriptors.
 *  2. Reads DSP write pointer (dsp_write_ptr) from the slot header.
 *  3. Reads newly arrived log bytes between host_read_ptr and dsp_write_ptr.
 *  4. Forwards log lines to debugger or breadcrumb log buffer.
 *  5. Writes updated host_read_ptr back to the slot header in DSP SRAM.
 *
 * @param[in,out] Ctx  Pointer to the SOF device context.
 * @param[in]     Core Zero-based DSP core index (0..3).
 */
VOID
SofIpc4DrainFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx,
    _In_    ULONG               Core
)
{
    ULONG debugWinOffset;
    ULONG slotOffset;
    ULONG dspWritePtr, hostReadPtr, avail, logBufSize, consumed;
    PUCHAR sramBase;

    /* Binary trace transport is not yet wired to a worker. Only a passive
     * caller with a verified firmware-provided debug window may drain it. */
    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc ||
        Core >= 4u || Core >= Ctx->HwDesc->CoresNum ||
        !Ctx->FwLogInitialized || Ctx->FwState != SOF_FW_BOOT_COMPLETE ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || KeAreAllApcsDisabled())
        return;

    sramBase = (PUCHAR)Ctx->Bar4VirtualAddress;
    debugWinOffset = Ctx->DebugWindowOffset;
    if (!debugWinOffset ||
        !pj_ipc4_mmio_span_valid(Ctx->Bar4Length, debugWinOffset, 65536u))
        return;

    /* Scan all descriptors each time. Do not guess a slot or use a stale
     * mapping after a firmware reset. Ambiguous descriptors are rejected. */
    slotOffset = 0;
    for (ULONG i = 0; i < 15u; ++i) {
        ULONG type = SofDspRead32(sramBase, debugWinOffset + i * 12u + 4u);
        if (type == (0x474F4C00u | Core)) {
            if (slotOffset != 0u) return;
            slotOffset = debugWinOffset + (i + 1u) * 4096u;
        }
    }
    if (!slotOffset) return;

    /* Read DSP write pointer and host read pointer */
    hostReadPtr = SofDspRead32(sramBase, slotOffset);
    dspWritePtr = SofDspRead32(sramBase, slotOffset + 4);
    logBufSize = 4096 - 8; /* 4088 bytes circular buffer */

    if (dspWritePtr >= logBufSize || hostReadPtr >= logBufSize) return;

    if (dspWritePtr == hostReadPtr) {
        return;
    }

    avail = (dspWritePtr > hostReadPtr) ?
            (dspWritePtr - hostReadPtr) :
            (logBufSize - hostReadPtr + dspWritePtr);

    consumed = 0;
    if (avail > 0 && avail <= logBufSize) {
        CHAR lineBuf[256];
        ULONG bytesToRead = (avail > 240) ? 240 : avail;

        for (ULONG b = 0; b < bytesToRead; b++) {
            ULONG bytePos = (hostReadPtr + b) % logBufSize;
            lineBuf[b] = (CHAR)SofDspRead8(sramBase, slotOffset + 8 + bytePos);
        }
        lineBuf[bytesToRead] = '\0';
        consumed = bytesToRead;

        /* Log newly arrived firmware trace */
        if (bytesToRead > 0) {
            CHAR bmsg[320];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[FW-c%u] %s\n", Core, lineBuf);
            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                SofLogBreadcrumb(bmsg);
            }
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", bmsg);
        }
    }

    /* Advance host read pointer in DSP SRAM */
    if (consumed != 0u) {
        hostReadPtr = (hostReadPtr + consumed) % logBufSize;
        SofDspWrite32(sramBase, slotOffset, hostReadPtr);
        Ctx->FwLogHostReadPtr[Core] = hostReadPtr;
        Ctx->FwLogSlotOffset[Core] = slotOffset;
    }
}

/**
 * @brief Configures and enables DSP firmware Zephyr logging over IPC4.
 *
 * Transmits system time synchronization (SOF_IPC4_FW_PARAM_SYSTEM_TIME) and log priority
 * configuration (SOF_IPC4_FW_PARAM_ENABLE_LOGS) to the Base FW module.
 *
 * @param[in,out] Ctx Pointer to the SOF device context.
 * @return NTSTATUS STATUS_SUCCESS if firmware logging was successfully armed.
 */
NTSTATUS
SofIpc4EnableFwLogs(
    _Inout_ PSOF_DEVICE_CONTEXT Ctx
)
{
    SOF_IPC_MESSAGE msg;
    pj_ipc4_packet packet = {0};
    pj_ipc4_status encoded;
    NTSTATUS status;
    LARGE_INTEGER sysTime;
    UINT64 usTime;

    struct {
        ULONG AgingTimerPeriod;
        ULONG FifoFullTimerPeriod;
        ULONG Enable;
        ULONG LogsPrioritiesMask[16];
    } logState;

    if (!Ctx || !Ctx->Bar4VirtualAddress || !Ctx->HwDesc) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || KeAreAllApcsDisabled())
        return STATUS_INVALID_DEVICE_STATE;
    /* A debug window must be established independently of trace commands. */
    if (!Ctx->DebugWindowOffset || !pj_ipc4_mmio_span_valid(Ctx->Bar4Length,
            Ctx->DebugWindowOffset, 65536u))
        return STATUS_NOT_SUPPORTED;
    if (Ctx->FwState != SOF_FW_BOOT_COMPLETE) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1. Synchronize system time with DSP firmware (SOF_IPC4_FW_PARAM_SYSTEM_TIME = 20) */
    KeQuerySystemTime(&sysTime);
    usTime = (UINT64)(sysTime.QuadPart / 10); /* Convert 100ns units to microseconds */

    RtlZeroMemory(&msg, sizeof(msg));
    encoded = pj_ipc4_large_config_set(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID,
        SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID, SOF_IPC4_FW_PARAM_SYSTEM_TIME,
        (const PJ_BYTE *)&usTime, sizeof(usTime), &packet);
    if (encoded != PJ_IPC4_VALID) return STATUS_INVALID_PARAMETER;
    msg.Primary = packet.primary;
    msg.Extension = packet.extension;
    msg.Payload = (PVOID)packet.payload;
    msg.PayloadSize = (ULONG)packet.payload_bytes;

    status = SofIpcSendMessage(Ctx, &msg, NULL, 500);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Warning: Failed to sync system time with FW (0x%08x)\n", status);
        return status;
    }

    /* 2. Configure log priorities and enable FW logs (SOF_IPC4_FW_PARAM_ENABLE_LOGS = 6) */
    RtlZeroMemory(&logState, sizeof(logState));
    logState.AgingTimerPeriod = 0x100;     /* 256 ms aging timer */
    logState.FifoFullTimerPeriod = 0x1000; /* 4096 ms FIFO timer */
    logState.Enable = 1;                   /* Enable logs */
    /* Enable levels: Critical, Error, Warning, Info + Infra, HAL, Module, Audio */
    logState.LogsPrioritiesMask[0] = 0x000001ef;

    RtlZeroMemory(&msg, sizeof(msg));
    encoded = pj_ipc4_large_config_set(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID,
        SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID, SOF_IPC4_FW_PARAM_ENABLE_LOGS,
        (const PJ_BYTE *)&logState, sizeof(logState), &packet);
    if (encoded != PJ_IPC4_VALID) return STATUS_INVALID_PARAMETER;
    msg.Primary = packet.primary;
    msg.Extension = packet.extension;
    msg.Payload = (PVOID)packet.payload;
    msg.PayloadSize = (ULONG)packet.payload_bytes;

    status = SofIpcSendMessage(Ctx, &msg, NULL, 500);
    if (NT_SUCCESS(status)) {
        Ctx->FwLogInitialized = TRUE;
        SofLogBreadcrumb("[SOF] DSP Firmware Zephyr logging enabled successfully\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] DSP Firmware Zephyr logging enabled.\n");
    } else {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "[SOF] Warning: Failed to enable FW logging over IPC4 (0x%08x)\n", status);
    }

    return status;
}
