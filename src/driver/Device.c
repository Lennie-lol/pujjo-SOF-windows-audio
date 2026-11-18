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
#include "Device.h"
#include "Loader.h"
#include "Topo.h"
#include "WaveRt.h"
#include "PortClsAdapter.h"
#include "HdaVerb.h"
#include "HdaBusInterface.h"
#include "SdwController.h"
#include "UsbOffload.h"
#include "UaolGateway.h"
#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>

NTSYSAPI NTSTATUS NTAPI ZwFlushKey(_In_ HANDLE KeyHandle);

static BOOLEAN SofHardwareDevelopmentAllowed(VOID)
{
#if defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)
    return TRUE;
#else
    return FALSE;
#endif
}

/* For a failed PrepareHardware, WDF may not call ReleaseHardware. */
static VOID SofUnmapFailedPreparedBars(_Inout_ PSOF_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Bar4VirtualAddress) {
        MmUnmapIoSpace(Ctx->Bar4VirtualAddress, Ctx->Bar4Length);
        Ctx->Bar4VirtualAddress = NULL;
        Ctx->Bar4Length = 0;
    }
    if (Ctx->Bar0VirtualAddress) {
        MmUnmapIoSpace(Ctx->Bar0VirtualAddress, Ctx->Bar0Length);
        Ctx->Bar0VirtualAddress = NULL;
        Ctx->Bar0Length = 0;
    }
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
    Ctx->NumPlaybackStreams = (gcap >> 12) & 0x0Fu;
    Ctx->PpCapOffset = pp;
    Ctx->SpbCapOffset = spb;
    Ctx->MlCapOffset = ml;
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] GCAP=0x%04x PP=0x%x SPIB=0x%x ML=0x%x (read-only)\n",
               (ULONG)gcap, pp, spb, ml);
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
    ULONG resourceCount;
    ULONG memoryBarIndex = 0;
    ULONG i;
    BUS_INTERFACE_STANDARD busInterface;
    char msg[256];

    UNREFERENCED_PARAMETER(ResourcesRaw);
    if (!SofHardwareDevelopmentAllowed()) return STATUS_NOT_SUPPORTED;

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
    if (NT_SUCCESS(WdfFdoQueryForInterface(Device,
                                           &GUID_BUS_INTERFACE_STANDARD,
                                           (PINTERFACE)&busInterface,
                                           sizeof(BUS_INTERFACE_STANDARD),
                                           1,
                                           NULL))) {
        PCI_COMMON_CONFIG pciConfig;
        ULONG bytesRead = busInterface.GetBusData(busInterface.Context,
                                                  PCI_WHICHSPACE_CONFIG,
                                                  &pciConfig,
                                                  0,
                                                  sizeof(PCI_COMMON_CONFIG));
        if (bytesRead >= sizeof(PCI_COMMON_CONFIG)) {
            ctx->VendorId = pciConfig.VendorID;
            ctx->DeviceId = pciConfig.DeviceID;
            ctx->RevisionId = pciConfig.RevisionID;

            /* Windows PCI/PnP owns command register policy. Never turn
             * on DMA bus mastering or memory decoding during discovery. */
            if ((pciConfig.Command & 0x0006u) != 0x0006u) {
                DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                           "[SOF] PCI MSE/BME not enabled (command=0x%04x); no change made\n",
                           pciConfig.Command);
            }
        }
        busInterface.InterfaceDereference(busInterface.Context);
    }

    /* Only the Intel Alder Lake-N 54C8 controller is in scope.
     * Never silently default to another Intel DSP when PCI config access fails. */
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

    /* Parse Translated Resources (BAR0 = HDA, BAR4 = DSP Shim/SRAM, Interrupts) */
    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (!desc)
            continue;

        if (desc->Type == CmResourceTypeMemory) {
            if (memoryBarIndex == 0) {
                /* BAR0: HD Audio Controller Registers */
                ctx->Bar0PhysicalAddress = desc->u.Memory.Start;
                ctx->Bar0Length = desc->u.Memory.Length;
                ctx->Bar0VirtualAddress = MmMapIoSpace(desc->u.Memory.Start,
                                                      desc->u.Memory.Length,
                                                      MmNonCached);
            } else if (memoryBarIndex == 1) {
                /* BAR4: DSP Shim, Mailbox, and SRAM */
                ctx->Bar4PhysicalAddress = desc->u.Memory.Start;
                ctx->Bar4Length = desc->u.Memory.Length;
                ctx->Bar4VirtualAddress = MmMapIoSpace(desc->u.Memory.Start,
                                                      desc->u.Memory.Length,
                                                      MmNonCached);
            }
            memoryBarIndex++;
        } else if (desc->Type == CmResourceTypeInterrupt) {
            ctx->InterruptVector = desc->u.Interrupt.Vector;
            ctx->InterruptIrql = (KIRQL)desc->u.Interrupt.Level;
            ctx->InterruptAffinity = desc->u.Interrupt.Affinity;
            ctx->IsMsi = (desc->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? TRUE : FALSE;

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] Interrupt Resource: Vector 0x%x, Level %u, MSI: %s\n",
                       ctx->InterruptVector,
                       ctx->InterruptIrql,
                       ctx->IsMsi ? "Yes" : "No");
        }
    }

    /* Require both mapped BAR windows before any register reads. Never
     * continue with half-initialized resources after MmMapIoSpace failure. */
    if (!ctx->Bar0VirtualAddress || !ctx->Bar4VirtualAddress ||
        ctx->Bar0Length < 0x80u || ctx->Bar4Length < sizeof(ULONG)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto PreparationFailed;
    }

    /* Discover HD-A controller capabilities without activating DSP processing */
    status = SofDiscoverHdaCaps(ctx);
    if (!NT_SUCCESS(status)) goto PreparationFailed;

    /*
     * Read codec wake/status without resetting HDA link or DSP state.
     * Recovery from stuck Immediate Command Busy (ICB) must be a separate
     * validated hardware operation, never part of PnP discovery.
     */
    if (ctx->Bar0VirtualAddress) {
        /* The caller is still in resource discovery: no CRST writes. */
        if (ctx->Bar0Length < SOF_HDA_WAKESTS + sizeof(USHORT) ||
            ctx->Bar0Length < SOF_HDA_REG_ICS + sizeof(USHORT)) {
            status = STATUS_INVALID_BUFFER_SIZE;
            goto PreparationFailed;
        }
        if (readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_REG_ICS) & SOF_HDA_ICS_ICB)
            SofLogBreadcrumb("[HDA] ICB busy during read-only discovery (no reset)\n");

        USHORT wakests = readw((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_WAKESTS);
        ctx->HdaCodecPresent = (wakests & SOF_HDA_WAKESTS_SDIN0) ? TRUE : FALSE;
        ctx->IDispCodecPresent = (wakests & SOF_HDA_WAKESTS_SDIN2) ? TRUE : FALSE;
        RtlStringCbPrintfA(msg, sizeof(msg),
            "[9a] HDA Codec Probe: WAKESTS=0x%04X -> HdaCodecPresent=%s, IDispCodecPresent=%s\n",
            wakests, ctx->HdaCodecPresent ? "TRUE" : "FALSE",
            ctx->IDispCodecPresent ? "TRUE" : "FALSE");
        SofLogBreadcrumb(msg);
    }

    {
        ULONG romSts = (ctx->Bar4VirtualAddress && ctx->HwDesc) ? SofReadRomStatus(ctx) : 0;
        ULONG fwSts = (ctx->Bar4VirtualAddress && ctx->HwDesc) ? SofReadFwStatus(ctx) : 0;

        RtlStringCbPrintfA(msg, sizeof(msg),
            "[9] Resources mapped: BAR0=%p (len 0x%x), BAR4=%p (len 0x%x), IRQ=0x%x (MSI=%s) | DSP ROM_STS=0x%08X (PP=0x%x, SPIB=0x%x, Streams: C%u/P%u)\n",
            ctx->Bar0VirtualAddress, ctx->Bar0Length,
            ctx->Bar4VirtualAddress, ctx->Bar4Length,
            ctx->InterruptVector, ctx->IsMsi ? "Yes" : "No",
            romSts, ctx->PpCapOffset, ctx->SpbCapOffset,
            ctx->NumCaptureStreams, ctx->NumPlaybackStreams);
        SofLogBreadcrumb(msg);
    }

    /* Initialize IPC state machine */
    status = SofIpcInitialize(ctx);
    if (!NT_SUCCESS(status)) goto PreparationFailed;

    /* Initialize default audio control states (0 dB, unmuted) */
    for (ULONG ep = 0; ep < SOF_ENDPOINT_COUNT; ep++) {
        ctx->DefaultVolumeLevelDb[ep][0] = 0;
        ctx->DefaultVolumeLevelDb[ep][1] = 0;
        ctx->DefaultMute[ep] = FALSE;
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
        if (NT_SUCCESS(nhltStatus) && nhltBuffer != NULL) {
            nhltStatus = SofNhltParse(nhltBuffer, nhltSize, &ctx->NhltInfo);
            if (!NT_SUCCESS(nhltStatus)) {
                RtlZeroMemory(&ctx->NhltInfo, sizeof(ctx->NhltInfo));
                SofLogBreadcrumb("[NHLT] Invalid table; no endpoint evidence accepted\n");
            }
            RtlStringCbPrintfA(msg, sizeof(msg),
                "[10] ACPI NHLT parsed: Endpoints=%u (DMIC=%u ch=%u, SSP=%u, SDW=%u)\n",
                ctx->NhltInfo.TotalEndpoints,
                ctx->NhltInfo.DmicEndpointsCount,
                ctx->NhltInfo.Dmic.ChannelCount,
                ctx->NhltInfo.SspEndpointsCount,
                ctx->NhltInfo.SdwEndpointsCount);
            SofLogBreadcrumb(msg);

            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] ACPI NHLT parsed: Total Endpoints=%u, DMIC=%u (Channels=%u), SSP=%u, SDW=%u\n",
                       ctx->NhltInfo.TotalEndpoints,
                       ctx->NhltInfo.DmicEndpointsCount,
                       ctx->NhltInfo.Dmic.ChannelCount,
                       ctx->NhltInfo.SspEndpointsCount,
                       ctx->NhltInfo.SdwEndpointsCount);
            SofNhltFreeAcpiTable(nhltBuffer);
        } else {
            SofLogBreadcrumb("[10] ACPI NHLT table not present (no endpoint evidence)\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] ACPI NHLT table not found; no endpoint evidence.\n");
        }
    }

    /* Enumerate SoundWire child PDOs (e.g. SOUNDWIRE\SDCA_PERIPHERAL) */
    status = SofSdwEnumerateChildren(ctx);
    if (!NT_SUCCESS(status)) {
        SofLogBreadcrumb("[SDW] SofSdwEnumerateChildren failed\n");
    }

    /*
     * Query PortCls configuration from driver parameters registry.
     * Missing settings leave optional endpoints and offload disabled.
     */
    ULONG armedTesting = 0;
    ULONG enablePortCls = 0;
    WDFKEY paramKey = NULL;
    NTSTATUS regStatus = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(),
        KEY_READ | KEY_WRITE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &paramKey
    );
    if (NT_SUCCESS(regStatus)) {
        UNICODE_STRING valArmed;
        UNICODE_STRING valPortCls;
        UNICODE_STRING valUsbOffload;
        ULONG enableUsbOffload = 0;
        RtlInitUnicodeString(&valArmed, L"ArmedTesting");
        RtlInitUnicodeString(&valPortCls, L"EnablePortCls");
        RtlInitUnicodeString(&valUsbOffload, L"EnableUsbOffload");
        WdfRegistryQueryULong(paramKey, &valArmed, &armedTesting);
        WdfRegistryQueryULong(paramKey, &valPortCls, &enablePortCls);
        WdfRegistryQueryULong(paramKey, &valUsbOffload, &enableUsbOffload);
        ctx->EnableUsbOffload = enableUsbOffload;
        WdfRegistryClose(paramKey);
    } else {
        ctx->EnableUsbOffload = 0;
    }

    ctx->ArmedTesting = armedTesting;
    ctx->EnablePortCls = enablePortCls;
    SofUaolInit(ctx);

    char safeMsg[160];
    RtlStringCbPrintfA(safeMsg, sizeof(safeMsg),
        "[10b] Safety Guards: CanaryArmed=%u, SafeBootPolicy=%u, EnablePortCls=%u\n",
        g_CanaryArmed ? 1 : 0, g_SafeBootPolicy, enablePortCls);
    SofLogBreadcrumb(safeMsg);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", safeMsg);

    if (g_CanaryArmed && enablePortCls != 0) {
        SofLogBreadcrumb("[10b] Canary Armed & PortCls enabled: Registering PortCls WaveRT Subdevices...\n");
        status = SofRegisterPortClsSubdevices(Device, ctx);
        if (!NT_SUCCESS(status)) {
            char smsg[128];
            RtlStringCbPrintfA(smsg, sizeof(smsg),
                "[10b] SofRegisterPortClsSubdevices failed: 0x%08x -> falling back to Safe Mode\n", status);
            SofLogBreadcrumb(smsg);
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "%s", smsg);
            ctx->PortClsRegistered = FALSE;
            status = STATUS_SUCCESS; /* Soft assert: ensure device and boot never fail */
        } else {
            ctx->PortClsRegistered = TRUE;
            SofLogBreadcrumb("[10b] SofRegisterPortClsSubdevices succeeded (Wave subdevice active)!\n");
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                       "[SOF] PortCls WaveRT subdevice registered successfully.\n");
        }
    } else {
        SofLogBreadcrumb("[10b] Safe Mode active: PortCls subdevices registration bypassed.\n");
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] Safe Mode: PortCls registration bypassed (CanaryArmed=%u, EnablePortCls=%u).\n",
                   g_CanaryArmed ? 1 : 0, enablePortCls);
        ctx->PortClsRegistered = FALSE;
    }

    ctx->HardwarePrepared = TRUE;
    SofLogBreadcrumb("[10a] Hardware prepared successfully (BAR0 & BAR4 mapped, IPC & NHLT ready)!\n");
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] Hardware preparation completed successfully.\n");
    return STATUS_SUCCESS;

PreparationFailed:
    /* Only BARs were acquired before these failure points; never leak them
     * if WDF does not deliver ReleaseHardware for failed preparation. */
    SofUnmapFailedPreparedBars(ctx);
    return status;
}

/**
 * @brief KMDF EvtDeviceReleaseHardware callback.
 *
 * Unregisters PortCls audio interfaces, stops active streams, cleans up IPC,
 * powers down the DSP cores, and unmaps BAR0 and BAR4 MMIO spaces.
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

    /* Unregister PortCls Subdevices */
    SofUnregisterPortClsSubdevices(Device, ctx);
    SofLogBreadcrumb("[Teardown] SofUnregisterPortClsSubdevices completed.\n");

    /* Free active audio streams */
    if (ctx->PlaybackStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->PlaybackStream);
        ctx->PlaybackStream = NULL;
    }
    if (ctx->CaptureStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->CaptureStream);
        ctx->CaptureStream = NULL;
    }
    for (ULONG i = 0; i < 3; i++) {
        if (ctx->HdmiStreams[i] != NULL) {
            SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->HdmiStreams[i]);
            ctx->HdmiStreams[i] = NULL;
        }
    }
    if (ctx->DmicStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->DmicStream);
        ctx->DmicStream = NULL;
    }
    if (ctx->SspPlaybackStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->SspPlaybackStream);
        ctx->SspPlaybackStream = NULL;
    }
    if (ctx->SspCaptureStream != NULL) {
        SofWaveRtFreeStream((PSOF_WAVERT_STREAM)ctx->SspCaptureStream);
        ctx->SspCaptureStream = NULL;
    }

    /* Cleanup IPC resources */
    SofIpcCleanup(ctx);

    /* Power down DSP hardware cleanly */
    SofPowerDownDsp(ctx);

    if (ctx->Bar0VirtualAddress) {
        MmUnmapIoSpace(ctx->Bar0VirtualAddress, ctx->Bar0Length);
        ctx->Bar0VirtualAddress = NULL;
        ctx->Bar0Length = 0;
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] BAR0 unmapped.\n");
    }

    if (ctx->Bar4VirtualAddress) {
        MmUnmapIoSpace(ctx->Bar4VirtualAddress, ctx->Bar4Length);
        ctx->Bar4VirtualAddress = NULL;
        ctx->Bar4Length = 0;
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[SOF] BAR4 unmapped.\n");
    }

    /* Uninitialize SoundWire controller context */
    SofSdwUninitController(ctx);

    ctx->HardwarePrepared = FALSE;
    return STATUS_SUCCESS;
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
    bootStatus = SofStartFirmwareBoot(ctx, NULL);
    if (!NT_SUCCESS(bootStatus)) {
        SofLogBreadcrumb("[Power] D0 entry aborted: firmware boot failed.\n");
        SofIpcDisableInterrupts(ctx);
        return bootStatus;
    }

    KeAcquireSpinLock(&ctx->IpcLock, &ipcIrql);
    ipcFaulted = ctx->IpcFaulted || ctx->FwState != SOF_FW_BOOT_COMPLETE;
    KeReleaseSpinLock(&ctx->IpcLock, ipcIrql);
    if (ipcFaulted) {
        SofIpcDisableInterrupts(ctx);
        return STATUS_DEVICE_NOT_READY;
    }
    ctx->CurrentPowerState = PowerDeviceD0;
    SofLogBreadcrumb("[Power] Firmware ready; audio graph and codec readiness are separate.\n");

    /* Immediately check and report jack presence on D0 entry */
    if (ctx->PortClsRegistered && ctx->PortEvents != NULL) {
        SofCheckAndNotifyJackPresence(ctx);
    }

    /* Start / re-arm Jack Poll Timer (1000ms periodic) */
    if (ctx->PortClsRegistered && ctx->PortEvents != NULL && ctx->JackPollTimer) {
        InterlockedExchange(&ctx->JackPolling, TRUE);
        WdfTimerStart(ctx->JackPollTimer, WDF_REL_TIMEOUT_IN_MS(1000));
        SofLogBreadcrumb("[Jack] Started JackPollTimer (1000ms periodic)\n");
    }

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
    if (!SofHardwareDevelopmentAllowed()) return STATUS_SUCCESS;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "[SOF] EvtDeviceD0Exit: Transitioning from D0 to Target State %d\n",
               TargetState);
    {
        char xmsg[128];
        RtlStringCbPrintfA(xmsg, sizeof(xmsg), "[Power] SofEvtDeviceD0Exit: TargetState=%d\n", TargetState);
        SofLogBreadcrumb(xmsg);
    }

    InterlockedExchange(&ctx->JackPolling, FALSE);
    if (ctx->JackPollTimer) WdfTimerStop(ctx->JackPollTimer, TRUE);

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

    /* Pause any active streams */
    if (ctx->PlaybackStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->PlaybackStream, KSSTATE_PAUSE);
    }
    if (ctx->CaptureStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->CaptureStream, KSSTATE_PAUSE);
    }
    for (ULONG i = 0; i < 3; i++) {
        if (ctx->HdmiStreams[i] != NULL) {
            SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->HdmiStreams[i], KSSTATE_PAUSE);
        }
    }
    if (ctx->DmicStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->DmicStream, KSSTATE_PAUSE);
    }
    if (ctx->SspPlaybackStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->SspPlaybackStream, KSSTATE_PAUSE);
    }
    if (ctx->SspCaptureStream != NULL) {
        SofWaveRtSetState((PSOF_WAVERT_STREAM)ctx->SspCaptureStream, KSSTATE_PAUSE);
    }

    /* Put HDA Codec to D3 to conserve power (Display Audio is power-managed by Intel Graphics) */
    if (ctx->HdaCodecPresent) {
        ULONG dummy;
        SofHdaSendVerb(ctx, HDA_VERB(0, 0x01, HDA_VERB_SET_POWER_STATE, 0x03), &dummy);
        SofLogBreadcrumb("[Power] HDA Codec set to D3\n");
    }

    /* Release / suspend USB Audio Offload */
    if (ctx->EnableUsbOffload) {
        SofUsbOffloadRelease(ctx);
    }

    /* Do not access the inherited Meteor Lake D0i3 register on Pujjo.
     * Invalidate the firmware session and wake waiters before power loss. */
    {
        KIRQL irql;
        NTSTATUS powerStatus;
        KeAcquireSpinLock(&ctx->IpcLock, &irql);
        ctx->IpcFaulted = TRUE;
        ctx->FwState = SOF_FW_BOOT_FAILED;
        KeSetEvent(&ctx->IpcReplyEvent, IO_NO_INCREMENT, FALSE);
        KeSetEvent(&ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&ctx->IpcLock, irql);
        ctx->CurrentPowerState = PowerDeviceUnspecified;
        SofIpcDisableInterrupts(ctx);
        powerStatus = SofPowerDownDsp(ctx);
        if (!NT_SUCCESS(powerStatus)) return powerStatus;
    }
    /* WDF_POWER_DEVICE_STATE is not DEVICE_POWER_STATE. Resume remains locked. */
    ctx->CurrentPowerState = PowerDeviceUnspecified;
    return STATUS_SUCCESS;
}

/**
 * @brief Periodic WDF timer callback executed at PASSIVE_LEVEL for jack polling.
 *
 * Checks ELD / Pin Sense registers on HDMI/DP display codecs and analog jacks,
 * firing IPortEvents notifications to Windows Audio service when headphones or
 * displays are plugged/unplugged. Re-arms itself for 1000ms.
 *
 * @param[in] Timer Handle to WDFTIMER.
 */
VOID
SofEvtJackPollTimer(
    _In_ WDFTIMER Timer
)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PSOF_DEVICE_CONTEXT ctx = GetDeviceContext(device);
    if (ctx && InterlockedCompareExchange(&ctx->JackPolling, 0, 0)) {
        if (ctx->PortEvents != NULL) {
            SofCheckAndNotifyJackPresence(ctx);
        }

        /* Passive timers cannot be periodic in KMDF; re-arm for next tick */
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1000));
    }
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

/**
 * @brief High-IRQL Interrupt Service Routine (ISR) for SOF Audio DSP.
 *
 * Checks if the interrupt was generated by the DSP doorbell registers. If so,
 * clears the edge and queues the DPC for deferred message processing.
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

    UNREFERENCED_PARAMETER(MessageID);
    if (!SofHardwareDevelopmentAllowed()) return FALSE;

    /* Check if this interrupt was asserted by the DSP */
    if (SofIpcIsrCheck(ctx)) {
        /* Queue DPC for deferred IPC packet processing */
        WdfInterruptQueueDpcForIsr(Interrupt);
        return TRUE;
    }

    return FALSE;
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

    UNREFERENCED_PARAMETER(AssociatedObject);
    if (!SofHardwareDevelopmentAllowed()) return;

    /* Process DSP IPC Mailbox reply or notification */
    SofIpcDpcHandler(ctx);
}
