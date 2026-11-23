/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file Device.c
 * @brief Device Initialization, MMIO BAR Mapping, and PnP/Power Handlers for SOF Windows Driver.
 *
 * Implements hardware resource preparation, PCI capability list traversal,
 * low-power S0-Idle (D0i3) hardware handshakes, interrupt servicing, and runtime power management.
 *
 * Hardware IP Highlights:
 * - HDA Extended Capabilities: Discovers Processing Pipe (PP, 0x3) and Software Position-in-Buffer (SPIB, 0x4).
 * - S0-Idle / D0i3 Handshake: Interacts with MTL_HDA_VS_D0I3C (BAR0: 0x1D4A) polling CIP (bit 0) and asserting I3 (bit 2).
 * - Multi-Link & SoundWire: Enumerates child peripheral devices discovered via ACPI or SoundWire bus registers.
 * - Jack & Display Presence: Employs dynamic polling via passive WDF timers to detect HDMI/DP hot-plug events.
 *
 * Copyright 2026 The SOF Authors
 * Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
 */

#include "Driver.h"
#include "PujjoBoardGuard.h"
#include "PujjoBoardId.h"
#include "Device.h"
#include "Loader.h"
#include "Topo.h"
#include "WaveRt.h"
#include "PujjoAudioBus.h"
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
#include "PujjoAnalog.h"
#endif
#include "HdaBusInterface.h"
#include "HdaVerb.h"
#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>

NTSYSAPI NTSTATUS NTAPI ZwFlushKey(_In_ HANDLE KeyHandle);

#if defined(PUJJO_DSP_BOOT_DIAGNOSTIC) && (!defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT) || \
    !defined(PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED) || defined(PUJJO_ENABLE_EXPERIMENTAL_AUDIO) || \
    defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG) || defined(PUJJO_REVIEWED_TOPOLOGY_NHLT))
#error DSP diagnostic must be boot-only and must not consume a replacement NHLT
#endif

static BOOLEAN SofHardwareDevelopmentAllowed(VOID)
{
#if defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)
    return TRUE;
#else
    return FALSE;
#endif
}

static VOID SofUnmapPreparedBars(_Inout_ PSOF_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Bar4VirtualAddress) {
        MmUnmapIoSpace(Ctx->Bar4VirtualAddress, Ctx->Bar4Length);
        Ctx->Bar4VirtualAddress = NULL;
    }
    if (Ctx->Bar0VirtualAddress) {
        MmUnmapIoSpace(Ctx->Bar0VirtualAddress, Ctx->Bar0Length);
        Ctx->Bar0VirtualAddress = NULL;
    }
    Ctx->Bar0Length = Ctx->Bar4Length = 0;
    Ctx->Bar0PhysicalAddress.QuadPart = Ctx->Bar4PhysicalAddress.QuadPart = 0;
}

static NTSTATUS SofMapPreparedBars(PSOF_DEVICE_CONTEXT ctx,
                                    PCM_PARTIAL_RESOURCE_DESCRIPTOR bars[2])
{
    ctx->Bar0PhysicalAddress = bars[0]->u.Memory.Start;
    ctx->Bar0Length = bars[0]->u.Memory.Length;
    ctx->Bar4PhysicalAddress = bars[1]->u.Memory.Start;
    ctx->Bar4Length = bars[1]->u.Memory.Length;
    ctx->Bar0VirtualAddress = MmMapIoSpace(ctx->Bar0PhysicalAddress, ctx->Bar0Length, MmNonCached);
    if (!ctx->Bar0VirtualAddress) return STATUS_INSUFFICIENT_RESOURCES;
    ctx->Bar4VirtualAddress = MmMapIoSpace(ctx->Bar4PhysicalAddress, ctx->Bar4Length, MmNonCached);
    if (!ctx->Bar4VirtualAddress) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

/**
 * @brief Traverses the Intel HDA extended capability linked list in BAR0.
 *
 * Discovers:
 * 1. Stream channel counts from GCAP (BAR0: 0x00).
 * 2. Processing Pipe Capability (PP, Cap ID 0x3): Enables GPROCEN (bit 30) and PIE (bit 31)
 *    to bridge host DMA channels into the DSP audio processing pipe.
 * 3. Software Position-in-Buffer Capability (SPIB, Cap ID 0x4): Enables low-latency DMA
 *    position reporting without PCI bus round-trip read penalties.
 * 4. Multi-Link Capability (ML, Cap ID 0x2): Discovers multi-link digital audio lines.
 *
 * @param[in,out] Ctx Pointer to device extension context.
 */
static NTSTATUS SofDiscoverHdaCaps(
    _In_ PSOF_DEVICE_CONTEXT Ctx
)
{
    PUCHAR bar0;
    ULONG offset, count = 0, i, cap, id;
    ULONG visited[SOF_HDA_MAX_CAPS];
    ULONG pp = 0, spb = 0, ml = 0;
    USHORT gcap;

    if (!Ctx) return STATUS_INVALID_PARAMETER;
    /* Invalidate previous results even if the next mapping is unavailable. */
    Ctx->PpCapOffset = Ctx->SpbCapOffset = Ctx->MlCapOffset = 0;
    Ctx->NumCaptureStreams = Ctx->NumPlaybackStreams = 0;
    Ctx->HdaDma64Bit = 0;
    if (!Ctx->Bar0VirtualAddress || Ctx->Bar0Length < 0x80u)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    bar0 = (PUCHAR)Ctx->Bar0VirtualAddress;
    /* GCAP is a 16-bit register, not the neighboring version bytes. */
    gcap = readw(bar0 + SOF_HDA_GCAP);
    if (gcap == 0xFFFFu) return STATUS_DEVICE_NOT_READY;
    offset = readw(bar0 + SOF_HDA_LLCH);

    /* An empty list is legitimate; a malformed list is not. Publish results
     * only after the entire bounded chain has been validated. */
    while (offset != 0) {
        if (count >= SOF_HDA_MAX_CAPS || offset == 0xFFFFu ||
            (offset & 3u) != 0 || offset < 0x80u ||
            offset > Ctx->Bar0Length - sizeof(ULONG))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        for (i = 0; i < count; ++i) {
            if (visited[i] == offset)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        visited[count++] = offset;
        cap = readl(bar0 + offset);
        if (cap == 0xFFFFFFFFu) return STATUS_DEVICE_NOT_READY;
        if (cap == 0) return STATUS_DEVICE_CONFIGURATION_ERROR;
        id = (cap & SOF_HDA_CAP_ID_MASK) >> SOF_HDA_CAP_ID_OFF;
        if (id == SOF_HDA_PP_CAP_ID) {
            if (pp || Ctx->Bar0Length - offset < SOF_HDA_REG_PP_PPCTL + sizeof(ULONG))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            pp = offset;
        } else if (id == SOF_HDA_SPIB_CAP_ID) {
            if (spb || Ctx->Bar0Length - offset < SOF_HDA_REG_CL_SPBFIFO_SPBFCCTL + sizeof(ULONG))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            spb = offset;
        } else if (id == SOF_HDA_ML_CAP_ID) {
            if (ml) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ml = offset;
        }
        offset = cap & SOF_HDA_CAP_NEXT_MASK;
    }
    Ctx->NumCaptureStreams = (gcap >> 8) & 0x0Fu;
    Ctx->HdaDma64Bit = (gcap & 1u) != 0; /* HDA GCAP.64OK */
    Ctx->NumPlaybackStreams = (gcap >> 12) & 0x0Fu;
    Ctx->PpCapOffset = pp;
    Ctx->SpbCapOffset = spb;
    Ctx->MlCapOffset = ml;
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] GCAP=0x%04x PP=0x%x SPIB=0x%x ML=0x%x (read-only)\n",
               (ULONG)gcap, pp, spb, ml);
    return STATUS_SUCCESS;
}

/** Check assigned PCI MMIO ranges before testing for overlap or mapping. */
static BOOLEAN SofMemoryRangeValid(PHYSICAL_ADDRESS Start, ULONG Length)
{
    return Start.QuadPart > 0 && Length != 0 &&
           (ULONGLONG)(Length - 1u) <= (ULONGLONG)(MAXLONGLONG - Start.QuadPart);
}

static BOOLEAN SofMemoryOverlaps(PCM_PARTIAL_RESOURCE_DESCRIPTOR First,
                                 PCM_PARTIAL_RESOURCE_DESCRIPTOR Second)
{
    LONGLONG a = First->u.Memory.Start.QuadPart;
    LONGLONG b = Second->u.Memory.Start.QuadPart;

    /* Callers have already validated both inclusive ranges. */
    return a <= b ? b - a < First->u.Memory.Length : a - b < Second->u.Memory.Length;
}

static BOOLEAN SofBarResourceValid(PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw,
                                    PCM_PARTIAL_RESOURCE_DESCRIPTOR Translated,
                                    ULONG MinimumLength)
{
    ULONG length = Raw->u.Memory.Length;

    return length >= MinimumLength && (length & (length - 1u)) == 0 &&
           (Raw->u.Memory.Start.QuadPart & (length - 1u)) == 0 &&
           (Translated->u.Memory.Start.QuadPart & (PAGE_SIZE - 1u)) == 0 &&
           !((Raw->Flags | Translated->Flags) &
             (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY));
}

static NTSTATUS SofSelectPciResources(
    WDFCMRESLIST RawList, WDFCMRESLIST TranslatedList,
    PHYSICAL_ADDRESS Bar0, PHYSICAL_ADDRESS Bar4,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Bars[2],
    PCM_PARTIAL_RESOURCE_DESCRIPTOR *InterruptRaw,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR *InterruptTranslated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR rawBars[2] = { NULL, NULL };
    ULONG count = WdfCmResourceListGetCount(TranslatedList);

    if (count != WdfCmResourceListGetCount(RawList) ||
        Bar0.QuadPart == Bar4.QuadPart)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (ULONG i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw = WdfCmResourceListGetDescriptor(RawList, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated = WdfCmResourceListGetDescriptor(TranslatedList, i);
        if (!raw || !translated)
            return STATUS_DEVICE_CONFIGURATION_ERROR;

        if (raw->Type == CmResourceTypeInterrupt || translated->Type == CmResourceTypeInterrupt) {
            if (raw->Type != translated->Type || *InterruptRaw ||
                ((raw->Flags ^ translated->Flags) &
                 (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                ((raw->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) &&
                 raw->u.MessageInterrupt.Raw.MessageCount != 1))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            *InterruptRaw = raw;
            *InterruptTranslated = translated;
        } else if (raw->Type == CmResourceTypeMemoryLarge ||
                   translated->Type == CmResourceTypeMemoryLarge) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        } else if (raw->Type == CmResourceTypeMemory || translated->Type == CmResourceTypeMemory) {
            if (raw->Type != translated->Type || raw->u.Memory.Length != translated->u.Memory.Length ||
                !SofMemoryRangeValid(raw->u.Memory.Start, raw->u.Memory.Length) ||
                !SofMemoryRangeValid(translated->u.Memory.Start, translated->u.Memory.Length))
                return STATUS_DEVICE_CONFIGURATION_ERROR;

            if (raw->u.Memory.Start.QuadPart == Bar0.QuadPart ||
                raw->u.Memory.Start.QuadPart == Bar4.QuadPart) {
                ULONG bar = raw->u.Memory.Start.QuadPart == Bar0.QuadPart ? 0u : 1u;
                if (rawBars[bar] || !SofBarResourceValid(raw, translated, bar ? 0x100000u : 0x4000u))
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                rawBars[bar] = raw;
                Bars[bar] = translated;
            }
        }
    }

    if (!Bars[0] || !Bars[1] || !*InterruptRaw)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* Also reject aliases from non-BAR descriptors, in either address space. */
    for (ULONG i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw = WdfCmResourceListGetDescriptor(RawList, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated = WdfCmResourceListGetDescriptor(TranslatedList, i);
        if (raw->Type != CmResourceTypeMemory)
            continue;
        for (ULONG bar = 0; bar < 2; ++bar) {
            if (raw != rawBars[bar] &&
                (SofMemoryOverlaps(raw, rawBars[bar]) ||
                 SofMemoryOverlaps(translated, Bars[bar])))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

/**
 * @brief KMDF EvtDevicePrepareHardware callback.
 *
 * Invoked by PnP manager when hardware resources are assigned. Maps BAR0 (HDA)
 * and BAR4 (DSP Shim/SRAM), queries PCI bus standard interface, parses ACPI NHLT,
 * discovers audio codecs, and sets up PortCls audio subdevices.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] ResourcesRaw List of raw hardware resources.
 * @param[in] ResourcesTranslated List of translated hardware resources.
 * @return NTSTATUS STATUS_SUCCESS or error code.
 */
NTSTATUS
SofEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS status;
    PHYSICAL_ADDRESS bar0Bus = {0}, bar4Bus = {0};
    PCM_PARTIAL_RESOURCE_DESCRIPTOR bars[2] = { NULL, NULL };
    PCM_PARTIAL_RESOURCE_DESCRIPTOR interruptRaw = NULL, interruptTranslated = NULL;
    WDF_INTERRUPT_CONFIG interruptConfig;
    BUS_INTERFACE_STANDARD busInterface;
    PCI_COMMON_CONFIG pciConfig = {0};
    char msg[256];

    if (!SofHardwareDevelopmentAllowed()) return STATUS_NOT_SUPPORTED;
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    if (!g_CanaryArmed || g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT)
        return STATUS_NOT_SUPPORTED;
#endif

    /* Pujjo-only fork. Fail closed before touching PCI registers. */
    if (!PujjoBoardIdentityMatches()) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[PUJJO] Non-Pujjo board or unreadable SMBIOS; refusing hardware access.\n");
        return STATUS_NOT_SUPPORTED;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDevicePrepareHardware entered.\n");
    SofLogBreadcrumb("[7] SofEvtDevicePrepareHardware entered!\n");

    /*
     * Identify hardware using a read-only PCI Standard Bus Interface query.
     * Do not change BME, MSE, or any PCI configuration while discovering.
     */
    status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&busInterface, sizeof(busInterface), 1, NULL);
    if (!NT_SUCCESS(status)) return status;
    ULONG bytesRead = busInterface.GetBusData(busInterface.Context, PCI_WHICHSPACE_CONFIG,
        &pciConfig, 0, sizeof(pciConfig));
    busInterface.InterfaceDereference(busInterface.Context);
    if ((pciConfig.HeaderType & 0x7fu) != 0 ||
        bytesRead < FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.SubSystemID) +
                    sizeof(pciConfig.u.type0.SubSystemID))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    ctx->VendorId = pciConfig.VendorID;
    ctx->DeviceId = pciConfig.DeviceID;
    ctx->RevisionId = pciConfig.RevisionID;
    if (pciConfig.Command == 0xffffu || !(pciConfig.Command & PCI_ENABLE_MEMORY_SPACE)) {
        SofLogBreadcrumb("[PCI] Memory decode unavailable before BAR discovery.\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Only the Intel Alder Lake-N 54C8 controller is in scope.
     * Never silently default to another Intel DSP when PCI config access fails. */
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    if (pciConfig.u.type0.SubVendorID != 0 || pciConfig.u.type0.SubSystemID != 0)
        return STATUS_NOT_SUPPORTED;
#endif
    if (ctx->VendorId != 0x8086 || ctx->DeviceId != 0x54C8) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[PUJJO] Expected Intel 8086:54C8; actual %04x:%04x. Refusing.\n",
                   ctx->VendorId, ctx->DeviceId);
        return STATUS_NOT_SUPPORTED;
    }

    ctx->HwDesc = SofGetHardwareDesc(ctx->VendorId, ctx->DeviceId);
    if (!ctx->HwDesc) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "[SOF] Unsupported hardware VID: 0x%04x, DID: 0x%04x\n",
                   ctx->VendorId, ctx->DeviceId);
        return STATUS_NOT_SUPPORTED;
    }

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[8] Hardware identified: VID=0x%04x DID=0x%04x -> %s\n",
        ctx->VendorId, ctx->DeviceId, ctx->HwDesc->PlatformName);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Platform identified: %s (VID 0x%04x, DID 0x%04x)\n",
               ctx->HwDesc->PlatformName, ctx->VendorId, ctx->DeviceId);

    /* Correlate PCI BAR bus addresses with the raw resource list. Use
     * each raw descriptor's translated counterpart for host MMIO. */
    ULONG high0, high4;
    if (!PujjoReadMemoryBar(pciConfig.u.type0.BaseAddresses, 0, &bar0Bus.LowPart, &high0) ||
        !PujjoReadMemoryBar(pciConfig.u.type0.BaseAddresses, 4, &bar4Bus.LowPart, &high4))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    bar0Bus.HighPart = (LONG)high0;
    bar4Bus.HighPart = (LONG)high4;
    status = SofSelectPciResources(ResourcesRaw, ResourcesTranslated, bar0Bus, bar4Bus,
                                  bars, &interruptRaw, &interruptTranslated);
    if (!NT_SUCCESS(status)) return status;

    status = SofMapPreparedBars(ctx, bars);
    if (!NT_SUCCESS(status)) goto PreparationFailed;

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, SofEvtInterruptIsr, SofEvtInterruptDpc);
    interruptConfig.AutomaticSerialization = FALSE;
    interruptConfig.InterruptRaw = interruptRaw;
    interruptConfig.InterruptTranslated = interruptTranslated;
    interruptConfig.EvtInterruptEnable = SofEvtInterruptEnable;
    interruptConfig.EvtInterruptDisable = SofEvtInterruptDisable;
    status = WdfInterruptCreate(Device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->WdfInterrupt);
    if (!NT_SUCCESS(status)) goto PreparationFailed;
    ctx->IsMsi = (interruptTranslated->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0;

    /* Discover HD-A controller capabilities without activating DSP processing */
    status = SofDiscoverHdaCaps(ctx);
    if (!NT_SUCCESS(status)) goto PreparationFailed;

    /* Controller status is observed without CRST writes. Wake-status
     * bits do not identify either ACPI peripheral on this board. */
    if (ctx->Bar0Length < SOF_HDA_REG_ICS + sizeof(USHORT)) {
        status = STATUS_INVALID_BUFFER_SIZE;
        goto PreparationFailed;
    }
    if (readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_REG_ICS) & SOF_HDA_ICS_ICB)
        SofLogBreadcrumb("[HDA] ICB busy during read-only discovery\n");

    {
        ULONG romSts = (ctx->Bar4VirtualAddress && ctx->HwDesc) ? SofReadRomStatus(ctx) : 0;

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[9] Resources mapped: BAR0=%p (len 0x%x), BAR4=%p (len 0x%x), MSI=%s | DSP ROM_STS=0x%08X (PP=0x%x, SPIB=0x%x, Streams: C%u/P%u)\n",
            ctx->Bar0VirtualAddress, ctx->Bar0Length,
            ctx->Bar4VirtualAddress, ctx->Bar4Length,
            ctx->IsMsi ? "Yes" : "No",
            romSts, ctx->PpCapOffset, ctx->SpbCapOffset,
            ctx->NumCaptureStreams, ctx->NumPlaybackStreams);
        SofLogBreadcrumb(msg);
    }

    /* Initialize IPC state machine */
    status = SofIpcInitialize(ctx);
    if (!NT_SUCCESS(status)) goto PreparationFailed;

    /* Start every endpoint muted. */
    for (ULONG ep = 0; ep < SOF_ENDPOINT_COUNT; ep++) {
        ctx->DefaultVolumeLevelDb[ep][0] = 0;
        ctx->DefaultVolumeLevelDb[ep][1] = 0;
        ctx->DefaultMute[ep] = TRUE;
    }

    /* Initialize display audio stream pointers and defaults */
    for (ULONG i = 0; i < 3; i++) {
        ctx->HdmiStreams[i] = NULL;
        ctx->DisplayConnected[i] = FALSE;
        RtlStringCbPrintfW(ctx->DisplayMonitorName[i], sizeof(ctx->DisplayMonitorName[i]),
                           L"Intel Display Audio %u", i + 1);
    }

    /* Query and parse ACPI NHLT table */
    {
        PVOID nhltBuffer = NULL;
        ULONG nhltSize = 0;
        NTSTATUS nhltStatus = SofNhltQueryAcpiTable(&nhltBuffer, &nhltSize);
        BOOLEAN topologySource = FALSE;
        if (nhltStatus == STATUS_NOT_FOUND || nhltStatus == STATUS_OBJECT_NAME_NOT_FOUND) {
            nhltStatus = SofNhltQueryPujjoFallback(nhltStatus, &nhltBuffer, &nhltSize);
            topologySource = NT_SUCCESS(nhltStatus);
        }
        if (NT_SUCCESS(nhltStatus) && nhltBuffer != NULL) {
            nhltStatus = SofNhltParse(nhltBuffer, nhltSize, &ctx->NhltInfo);
            if (NT_SUCCESS(nhltStatus)) {
                ctx->NhltTable = nhltBuffer;
                ctx->NhltTableSize = nhltSize;
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[NHLT] Valid %s table: %u endpoints (DMIC=%u, SSP=%u, SDW=%u)\n",
                    topologySource ? "Pujjo topology" : "ACPI", ctx->NhltInfo.TotalEndpoints, ctx->NhltInfo.DmicEndpointsCount,
                    ctx->NhltInfo.SspEndpointsCount, ctx->NhltInfo.SdwEndpointsCount);
            } else {
                SofNhltFreeAcpiTable(nhltBuffer);
                RtlStringCbPrintfA(msg, sizeof(msg),
                    "[NHLT] Invalid ACPI table: 0x%08x\n", nhltStatus);
            }
        } else {
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[NHLT] ACPI table unavailable: 0x%08x\n", nhltStatus);
        }
        SofLogBreadcrumb(msg);
    }

    /* These settings request an audio adapter after firmware boot. */
    ctx->ArmedTesting = 0;
    ctx->EnablePortCls = 0;
    {
        WDFKEY paramKey = NULL;
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
                WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            UNICODE_STRING valArmed, valPortCls;
            RtlInitUnicodeString(&valArmed, L"ArmedTesting");
            RtlInitUnicodeString(&valPortCls, L"EnablePortCls");
            WdfRegistryQueryULong(paramKey, &valArmed, &ctx->ArmedTesting);
            WdfRegistryQueryULong(paramKey, &valPortCls, &ctx->EnablePortCls);
            WdfRegistryClose(paramKey);
        }
    }

    ctx->HardwarePrepared = TRUE;
    SofLogBreadcrumb("[10a] BAR resources and host IPC state prepared\n");
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Mapped hardware resources.\n");
    return STATUS_SUCCESS;

PreparationFailed:
    /* KMDF retires any interrupt created in this PrepareHardware pass
     * after ReleaseHardware. It was never enabled on this failure path. */
    SofUnmapPreparedBars(ctx);
    return status;
}

/* Continue quiescing other endpoints after a failure. Report the first error
 * and preserve any buffers which WaveRT could not prove safe to release. */
static NTSTATUS SofQuiesceDeviceStreams(PSOF_DEVICE_CONTEXT Ctx, BOOLEAN Release)
{
    PVOID *slots[] = {
        &Ctx->PlaybackStream, &Ctx->CaptureStream,
        &Ctx->HdmiStreams[0], &Ctx->HdmiStreams[1], &Ctx->HdmiStreams[2],
        &Ctx->SspPlaybackStream, &Ctx->SspCaptureStream
    };
    NTSTATUS status = STATUS_SUCCESS;

    for (ULONG i = 0; i < RTL_NUMBER_OF(slots); ++i) {
        PSOF_WAVERT_STREAM stream = (PSOF_WAVERT_STREAM)*slots[i];
        NTSTATUS streamStatus;
        if (!stream) continue;
        if (Release) {
            streamStatus = SofWaveRtFreeStream(stream);
            *slots[i] = NULL;
        } else {
            streamStatus = SofWaveRtSetState(stream, KSSTATE_PAUSE);
        }
        if (NT_SUCCESS(status) && !NT_SUCCESS(streamStatus)) status = streamStatus;
    }
    return status;
}

/**
 * @brief KMDF EvtDeviceReleaseHardware callback.
 *
 * Disconnects remaining audio interfaces, drains interrupt work, releases IPC
 * state, and unmaps BAR0 and BAR4 after the framework disconnects the IRQ.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] ResourcesTranslated List of translated hardware resources.
 * @return NTSTATUS STATUS_SUCCESS.
 */
NTSTATUS
SofEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    if (!SofHardwareDevelopmentAllowed()) return STATUS_SUCCESS;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] SofEvtDeviceReleaseHardware starting...\n");
    SofLogBreadcrumb("[Teardown] SofEvtDeviceReleaseHardware starting...\n");

    NTSTATUS status = SofDisconnectAudioChild(ctx);
    NTSTATUS streamStatus = SofQuiesceDeviceStreams(ctx, TRUE);
    if (NT_SUCCESS(status)) status = streamStatus;
    /* KMDF has disconnected the ISR. Drain any deferred work even on a
     * failed D0 transition where the normal pre-disable callback was skipped. */
    if (ctx->WdfInterrupt)
        KeFlushQueuedDpcs();
    SofIpcCleanup(ctx);
    SofNhltFreeAcpiTable(ctx->NhltTable);
    ctx->NhltTable = NULL;
    ctx->NhltTableSize = 0;
    RtlZeroMemory(&ctx->NhltInfo, sizeof(ctx->NhltInfo));

    ctx->WdfInterrupt = NULL; /* KMDF owns deletion after this callback. */
    ctx->IsMsi = FALSE;
    ctx->IrqMaskNeeded = FALSE;
    SofUnmapPreparedBars(ctx);

    ctx->HardwarePrepared = FALSE;
    return status;
}

/**
 * @brief KMDF EvtDeviceD0Entry callback.
 *
 * Invoked when transitioning into the working power state (D0).
 * Exits D0i3, unmasks IPC interrupts, cold boots or restores DSP firmware,
 * validates on-demand audio pipelines, probes codecs, and arms timers.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] PreviousState Power state prior to entering D0.
 * @return NTSTATUS STATUS_SUCCESS on success.
 */
NTSTATUS
SofEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)
    /* Default development build is NEVER allowed to power or boot Pujjo DSP.
       This is not an installable or hardware-test-ready audio driver. */
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    return STATUS_NOT_SUPPORTED;
#else
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    char msg[128];

    RtlStringCbPrintfA(msg, sizeof(msg),
        "[11] SofEvtDeviceD0Entry: Entering D0 from state %d\n", PreviousState);
    SofLogBreadcrumb(msg);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] EvtDeviceD0Entry: Entering D0 from State %d\n",
               PreviousState);

    KIRQL ipcIrql;
    SOF_FW_BOOT_STATE initialFwState;
    BOOLEAN ipcFaulted;
    NTSTATUS bootStatus;

    KeAcquireSpinLock(&ctx->IpcLock, &ipcIrql);
    initialFwState = ctx->FwState;
    ipcFaulted = ctx->IpcFaulted || ctx->IpcTxBusy;
    KeReleaseSpinLock(&ctx->IpcLock, ipcIrql);

    /* No retained-session assumption: resume needs a reviewed cAVS power,
     * DMA and IPC ownership restoration path. Do not probe ACE registers on
     * this ADL-N target or attempt a cold boot on an occupied DSP. */
    if (ipcFaulted || initialFwState != SOF_FW_BOOT_NOT_STARTED ||
        PreviousState != WdfPowerDeviceD3Final)
        return STATUS_DEVICE_NOT_READY;
    if (!ctx->HardwarePrepared || !ctx->HwDesc ||
        !ctx->Bar0VirtualAddress || !ctx->Bar4VirtualAddress)
        return STATUS_DEVICE_NOT_READY;

    /* The loader is the sole owner of the cold firmware boot sequence.
     * No speculative HDMI/HDA/SSP graph creation or codec programming here. */
#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC
    if (!g_CanaryArmed || g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT ||
        ctx->EnablePortCls != 0)
        return STATUS_NOT_SUPPORTED;
    /* Consume the in-memory authorization too, before any firmware boot.
     * DriverEntry already consumed persistent authorization. */
    g_CanaryArmed = FALSE;
#endif
    /* Re-read at D0 entry. The bus owns these PCI command bits and their
     * state during PrepareHardware need not be their state at DMA start. */
    BUS_INTERFACE_STANDARD busInterface;
    USHORT command = 0;
    bootStatus = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&busInterface, sizeof(busInterface), 1, NULL);
    if (!NT_SUCCESS(bootStatus)) return bootStatus;
    ULONG bytesRead = busInterface.GetBusData(busInterface.Context, PCI_WHICHSPACE_CONFIG,
        &command, FIELD_OFFSET(PCI_COMMON_CONFIG, Command), sizeof(command));
    busInterface.InterfaceDereference(busInterface.Context);
    const USHORT required = PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER;
    if (bytesRead != sizeof(command) || command == 0xffffu || (command & required) != required) {
        SofLogBreadcrumb("[PCI] D0 requires memory decode and bus mastering.\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    bootStatus = SofStartFirmwareBoot(ctx, NULL);
    if (!NT_SUCCESS(bootStatus)) {
        RtlStringCbPrintfA(msg, sizeof(msg), "[Power] D0 entry aborted: firmware boot status=0x%08x\n", bootStatus);
        SofLogBreadcrumb(msg);
        return bootStatus;
    }

    KeAcquireSpinLock(&ctx->IpcLock, &ipcIrql);
    ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;
    KeReleaseSpinLock(&ctx->IpcLock, ipcIrql);
    if (ipcFaulted) {
        SofIpcDisableInterrupts(ctx);
        SofIpcQuiesce(ctx);
        if (ctx->DspNeedsPowerDown && !SofDeviceRemoved(ctx)) {
            NTSTATUS powerStatus = SofPowerDownDsp(ctx);
            if (!NT_SUCCESS(powerStatus)) return powerStatus;
        }
        if (!NT_SUCCESS(SofRestoreBootController(ctx)))
            SofLogBreadcrumb("[HDA] Controller rollback could not be confirmed.\n");
        return STATUS_DEVICE_NOT_READY;
    }

    if (g_CanaryArmed && ctx->ArmedTesting != 0 && ctx->EnablePortCls != 0) {
        NTSTATUS portStatus = SofPublishAudioChild(ctx);
        if (portStatus == STATUS_NOT_FOUND) {
            SofLogBreadcrumb("[Power] No qualifying DMIC format. Audio child omitted.\n");
        } else if (!NT_SUCCESS(portStatus)) {
            SofLogBreadcrumb("[Power] Requested PortCls registration failed.\n");
            SofIpcDisableInterrupts(ctx);
            SofIpcQuiesce(ctx);
            NTSTATUS powerStatus = SofDeviceRemoved(ctx) ?
                STATUS_DEVICE_REMOVED : SofPowerDownDsp(ctx);
            if (!NT_SUCCESS(powerStatus)) return powerStatus;
            NTSTATUS restoreStatus = SofRestoreBootController(ctx);
            return NT_SUCCESS(restoreStatus) ? portStatus : restoreStatus;
        }
    }
    ctx->CurrentPowerState = PowerDeviceD0;
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
    if (g_CanaryArmed && ctx->ArmedTesting && ctx->EnablePortCls)
        SofPublishApprovedAnalogChildren(ctx);
#endif
    SofLogBreadcrumb("[Power] DSP firmware reported FW_READY.\n");

    /* Start Watchdog Health Timer (10s countdown to confirm healthy D0 uptime) */
    if (ctx->WatchdogHealthTimer) {
        WdfTimerStart(ctx->WatchdogHealthTimer, WDF_REL_TIMEOUT_IN_MS(10000));
        SofLogBreadcrumb("[Watchdog] Started WatchdogHealthTimer (10s countdown to health confirmation)\n");
    }

    /* Check for diagnostic DSP panic/core dump trigger */
    {
        WDFKEY paramKey = NULL;
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            ULONG triggerDump = 0;
            UNICODE_STRING valTrigger;
            RtlInitUnicodeString(&valTrigger, L"TriggerDspPanicDump");
            if (NT_SUCCESS(WdfRegistryQueryULong(paramKey, &valTrigger, &triggerDump)) && triggerDump != 0) {
                SofLogBreadcrumb("[Watchdog] Diagnostic TriggerDspPanicDump=1 detected! Capturing DSP core dump...\n");
                SofDspDumpCore(ctx, "DIAGNOSTIC_TRIGGER");
                ULONG zero = 0;
                WdfRegistryAssignULong(paramKey, &valTrigger, zero);
                ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
                SofLogBreadcrumb("[Watchdog] Diagnostic core dump completed; TriggerDspPanicDump reset to 0.\n");
            }
            WdfRegistryClose(paramKey);
        }
    }

    /* Pujjo USB offload is not a verified part of cold boot. */
    SofLogBreadcrumb("[17] SofEvtDeviceD0Entry completed successfully!\n");
    return STATUS_SUCCESS;
#endif /* PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT */
}

/**
 * @brief KMDF EvtDeviceD0Exit callback.
 *
 * Invoked when transitioning from D0 into a low-power state (D3, S0ix, S3, S4).
 * Stops the watchdog timer, pauses active streams, puts the HDA codec into D3,
 * releases USB audio offload, masks interrupts, and powers down the DSP.
 * Retained-session resume is deliberately not qualified.
 *
 * @param[in] Device Handle to WDFDEVICE.
 * @param[in] TargetState Low-power target state (PowerDeviceD3, etc.).
 * @return NTSTATUS STATUS_SUCCESS.
 */
NTSTATUS
SofEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    if (!SofHardwareDevelopmentAllowed() || !ctx->HardwarePrepared) return STATUS_SUCCESS;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] EvtDeviceD0Exit: Transitioning from D0 to Target State %d\n",
               TargetState);
    {
        char xmsg[128];
        RtlStringCbPrintfA(xmsg, sizeof(xmsg), "[Power] SofEvtDeviceD0Exit: TargetState=%d\n", TargetState);
        SofLogBreadcrumb(xmsg);
    }

    /* Stop Watchdog Health Timer when leaving D0 */
    if (ctx->WatchdogHealthTimer) {
        WdfTimerStop(ctx->WatchdogHealthTimer, TRUE);
        SofLogBreadcrumb("[Watchdog] Stopped WatchdogHealthTimer\n");

        /* If cleanly leaving D0, reset ConsecutiveCrashCount */
        WDFKEY paramKey = NULL;
        UNICODE_STRING valCrash;
        RtlInitUnicodeString(&valCrash, L"ConsecutiveCrashCount");
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
            ULONG zero = 0;
            WdfRegistryAssignULong(paramKey, &valCrash, zero);
            ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
            WdfRegistryClose(paramKey);
            SofLogBreadcrumb("[Watchdog] D0Exit clean: Reset ConsecutiveCrashCount = 0.\n");
        }
    }

    NTSTATUS streamStatus = SofDisconnectAudioChild(ctx);
    NTSTATUS otherStatus = SofQuiesceDeviceStreams(ctx, FALSE);
    if (NT_SUCCESS(streamStatus)) streamStatus = otherStatus;

    /* Stop device IRQs and drain the DPC and IPC senders before core reset. */
    ctx->CurrentPowerState = PowerDeviceUnspecified;
    SofIpcDisableInterrupts(ctx);
    SofIpcQuiesce(ctx);
    if (ctx->DspNeedsPowerDown && !SofDeviceRemoved(ctx)) {
        NTSTATUS powerStatus = SofPowerDownDsp(ctx);
        if (!NT_SUCCESS(powerStatus)) return powerStatus;
    }
    if (!NT_SUCCESS(streamStatus)) return streamStatus;
    return SofRestoreBootController(ctx);
}

/**
 * @brief Watchdog health confirmation timer callback executed 10s after D0Entry.
 *
 * If the system remains stable in D0 for 10 seconds without a bugcheck or kernel panic,
 * this callback confirms health and resets ConsecutiveCrashCount to 0 in the registry.
 *
 * @param[in] Timer Handle to WDFTIMER.
 */
VOID
SofEvtWatchdogHealthTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    WDFKEY paramKey = NULL;
    UNICODE_STRING valCrash;
    UNREFERENCED_PARAMETER(device);

    RtlInitUnicodeString(&valCrash, L"ConsecutiveCrashCount");
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey))) {
        ULONG zero = 0;
        WdfRegistryAssignULong(paramKey, &valCrash, zero);
        ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
        WdfRegistryClose(paramKey);
        SofLogBreadcrumb("[Watchdog] Health confirmed (10s in D0)! Reset ConsecutiveCrashCount = 0.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Watchdog: Driver healthy (10s in D0). Reset ConsecutiveCrashCount to 0.\n");
    }
}

VOID
SofEvtDeviceSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);

    /* The device can already be absent. Close the software IRQ gate without
     * touching registers, and release any outstanding IPC waiters. */
    InterlockedExchange(&ctx->SurpriseRemoved, 1);
    SofIpcDisableInterrupts(ctx);
    SofIpcQuiesce(ctx);
}

NTSTATUS
SofEvtInterruptEnable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE Device)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);

    ctx->IrqConnected = TRUE;
    /* ROM polling initially owns the doorbells. The guarded loader arms IPC. */
    return STATUS_SUCCESS;
}

NTSTATUS
SofEvtInterruptDisable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE Device)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);

    /* The pre-disable power callback masks and drains at PASSIVE_LEVEL.
     * This also closes the software gate on failed power transitions. */
    if (ctx->IrqMaskNeeded)
        SofIpcMaskInterrupt(ctx);
    ctx->IrqConnected = FALSE;
    ctx->IrqArmed = FALSE;
    ctx->IrqPending = FALSE;
    ctx->IrqMaskFailed = FALSE;
    return STATUS_SUCCESS;
}

/**
 * @brief High-IRQL Interrupt Service Routine (ISR) for SOF Audio DSP.
 *
 * Checks DSP interrupt ownership, masks the source, and queues the DPC.
 *
 * @param[in] Interrupt Handle to WDFINTERRUPT.
 * @param[in] MessageID MSI message index (unused for line-based IRQs).
 * @return BOOLEAN TRUE if the interrupt belonged to this device; FALSE otherwise.
 */
BOOLEAN
SofEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    if (!ctx->IrqArmed || ctx->IrqPending || SofDeviceRemoved(ctx) ||
        (ctx->IsMsi && MessageID != 0) || !SofIpcIsrCheck(ctx))
        return FALSE;

    ctx->IrqPending = TRUE;
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

/**
 * @brief Deferred Procedure Call (DPC) processing IPC messages at DISPATCH_LEVEL.
 *
 * @param[in] Interrupt Handle to WDFINTERRUPT.
 * @param[in] AssociatedObject Associated WDF object.
 */
VOID
SofEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
)
{
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    BOOLEAN pending;
    BOOLEAN handled;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(AssociatedObject);
    WdfInterruptAcquireLock(Interrupt);
    pending = ctx->IrqArmed && ctx->IrqPending && !SofDeviceRemoved(ctx);
    handled = !ctx->IrqMaskFailed;
    WdfInterruptReleaseLock(Interrupt);
    if (!pending)
        return;

    handled = handled && SofIpcDpcHandler(ctx);
    KeAcquireSpinLock(&ctx->IpcLock, &oldIrql);
    handled = handled && !ctx->IpcFaulted;
    KeReleaseSpinLock(&ctx->IpcLock, oldIrql);

    WdfInterruptAcquireLock(Interrupt);
    ctx->IrqPending = FALSE;
    ctx->IrqMaskFailed = FALSE;
    if (ctx->IrqArmed && (!handled || !SofIpcRearmInterrupt(ctx))) {
        ctx->IrqArmed = FALSE;
        SofIpcMaskInterrupt(ctx);
        handled = FALSE;
    }
    WdfInterruptReleaseLock(Interrupt);

    if (!handled) {
        KeAcquireSpinLock(&ctx->IpcLock, &oldIrql);
        ctx->IpcFaulted = TRUE;
        ctx->FwState = SOF_FW_BOOT_FAILED;
        KeSetEvent(&ctx->IpcReplyEvent, IO_NO_INCREMENT, FALSE);
        KeSetEvent(&ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&ctx->IpcLock, oldIrql);
    }
}
