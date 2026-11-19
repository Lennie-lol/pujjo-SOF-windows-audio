/* SPDX-License-Identifier: BSD-3-Clause */
#include <initguid.h>
#include "PujjoAudioBus.h"
#include "Device.h"
#include "WaveRt.h"
#include "PujjoDmaPlan.h"
#include "PujjoIpc4Protocol.h"

#define AUDIO_INTERFACE_TAG 'iAuP'

#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
static VOID AudioReference(PVOID Context)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    WdfObjectReferenceWithTag(ctx->WdfDevice, (PVOID)(ULONG_PTR)AUDIO_INTERFACE_TAG);
}

static VOID AudioDereference(PVOID Context)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    WdfObjectDereferenceWithTag(ctx->WdfDevice, (PVOID)(ULONG_PTR)AUDIO_INTERFACE_TAG);
}

#endif

static VOID AudioLock(PSOF_DEVICE_CONTEXT Ctx)
{
    KeWaitForSingleObject(&Ctx->AudioMutex, Executive, KernelMode, FALSE, NULL);
}

static VOID AudioUnlock(PSOF_DEVICE_CONTEXT Ctx)
{
    KeReleaseMutex(&Ctx->AudioMutex, FALSE);
}

static VOID AudioSetOnline(PSOF_DEVICE_CONTEXT Ctx, BOOLEAN Online)
{
    KIRQL irql;
    KeAcquireSpinLock(&Ctx->AudioPositionLock, &irql);
    Ctx->AudioOnline = Online;
    KeReleaseSpinLock(&Ctx->AudioPositionLock, irql);
}

static VOID AudioSetPositionEnabled(PSOF_DEVICE_CONTEXT Ctx, BOOLEAN Enabled)
{
    KIRQL irql;
    KeAcquireSpinLock(&Ctx->AudioPositionLock, &irql);
    Ctx->AudioPositionEnabled = Enabled;
    KeReleaseSpinLock(&Ctx->AudioPositionLock, irql);
}

#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
static NTSTATUS AudioGetFormat(PVOID Context, PUJJO_AUDIO_FORMAT *Format)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    AudioLock(ctx);
    if (ctx->AudioOnline) {
        *Format = ctx->AudioFormat;
        status = STATUS_SUCCESS;
    }
    AudioUnlock(ctx);
    return status;
}

static NTSTATUS AudioAllocate(PVOID Context, ULONG RequestedBytes, PUJJO_AUDIO_BUFFER *Buffer)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    PSOF_WAVERT_STREAM stream;
    SOF_AUDIO_FORMAT_PARAMS format;
    ULONG size, periodMs;
    NTSTATUS status;

    RtlZeroMemory(Buffer, sizeof(*Buffer));
    AudioLock(ctx);
    if (!ctx->AudioOnline || ctx->AudioDmaFaulted) {
        status = STATUS_DEVICE_NOT_READY;
        goto Done;
    }
    if (ctx->DmicStream) {
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }

    SofIpc4BuildDefaultStereoFormat(ctx->AudioFormat.Rate, ctx->AudioFormat.ContainerBits, &format);
    format.ValidBitDepth = ctx->AudioFormat.ValidBits;
    if (!pj_pcm_notification_ring(format.SamplingFrequency, format.ChannelsCount,
            format.BitDepth, format.ValidBitDepth, RequestedBytes, &size, &periodMs)) {
        status = STATUS_INVALID_BUFFER_SIZE;
        goto Done;
    }

    status = SofWaveRtCreateStream(ctx, SOF_ENDPOINT_DMIC_PDM, TRUE, 0, &format, size,
                                   periodMs, &stream);
    if (!NT_SUCCESS(status)) goto Done;
    stream->Mdl = IoAllocateMdl(stream->KernelBuffer, stream->BufferSizeBytes,
                               FALSE, FALSE, NULL);
    if (!stream->Mdl) {
        if (!NT_SUCCESS(SofWaveRtFreeStream(stream))) ctx->AudioDmaFaulted = TRUE;
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    MmBuildMdlForNonPagedPool(stream->Mdl);
    /* Keep the WDF DMA allocation alive until PortCls returns its MDL. */
    WdfObjectReference(ctx->DmaEnabler);
    WdfObjectReference(stream->RingAllocation.CommonBuffer);
    WdfObjectReference(stream->BdlAllocation.CommonBuffer);
    stream->BuffersLeased = TRUE;
    ctx->DmicStream = stream;
    Buffer->Stream = stream;
    Buffer->Mdl = stream->Mdl;
    Buffer->Bytes = stream->BufferSizeBytes;
    AudioSetPositionEnabled(ctx, TRUE);
    /* The pages already have a cache type from the DMA adapter. */
    Buffer->CacheType = MmCached;
Done:
    AudioUnlock(ctx);
    return status;
}

static VOID AudioFree(PVOID Context, PVOID Stream)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    PSOF_WAVERT_STREAM stream = Stream;
    AudioLock(ctx);
    if (ctx->DmicStream == stream && stream) {
        AudioSetPositionEnabled(ctx, FALSE);
        ctx->DmicStream = NULL;
        if (!NT_SUCCESS(SofWaveRtFreeStream(stream))) ctx->AudioDmaFaulted = TRUE;
    }
    AudioUnlock(ctx);
}

static NTSTATUS AudioSetState(PVOID Context, PVOID Stream, ULONG State)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    PSOF_WAVERT_STREAM stream = Stream;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;

    if (State > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    AudioLock(ctx);
    if (ctx->AudioOnline && ctx->DmicStream == stream && stream &&
        (State != KSSTATE_RUN || !ctx->AudioDmaFaulted)) {
        AudioSetPositionEnabled(ctx, FALSE);
        if (State != KSSTATE_RUN && stream->State == KSSTATE_RUN)
            status = SofWaveRtSetMute(stream, TRUE);
        else
            status = STATUS_SUCCESS;

        NTSTATUS transition = SofWaveRtSetState(stream, (KSSTATE)State);
        if (NT_SUCCESS(status)) status = transition;
        if (NT_SUCCESS(status) && State == KSSTATE_RUN) {
            status = SofWaveRtSetMute(stream, FALSE);
            if (!NT_SUCCESS(status) && !NT_SUCCESS(SofWaveRtSetState(stream, KSSTATE_STOP)))
                ctx->AudioDmaFaulted = TRUE;
        }
        if (NT_SUCCESS(status) && stream->DescriptorPublished)
            AudioSetPositionEnabled(ctx, TRUE);
    }
    AudioUnlock(ctx);
    return status;
}

static NTSTATUS AudioPosition(PVOID Context, PVOID Stream, PULONG Position)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    KIRQL irql;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    KeAcquireSpinLock(&ctx->AudioPositionLock, &irql);
    if (ctx->AudioOnline && ctx->AudioPositionEnabled)
        status = SofWaveRtGetPosition(Stream, Position);
    KeReleaseSpinLock(&ctx->AudioPositionLock, irql);
    return status;
}

static NTSTATUS AudioNotification(PVOID Stream, PKEVENT Event, BOOLEAN Add)
{
    return Add ? SofWaveRtRegisterNotificationEvent(Stream, Event) :
                 SofWaveRtUnregisterNotificationEvent(Stream, Event);
}

#endif

NTSTATUS SofDisconnectAudioChild(PSOF_DEVICE_CONTEXT Ctx)
{
    NTSTATUS status = STATUS_SUCCESS;
    AudioLock(Ctx);
    AudioSetOnline(Ctx, FALSE);
    AudioSetPositionEnabled(Ctx, FALSE);
    if (Ctx->DmicStream)
        status = SofWaveRtDetachStream(Ctx->DmicStream);
    AudioUnlock(Ctx);
    return status;
}

#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
static NTSTATUS AudioSelectFormat(PSOF_DEVICE_CONTEXT Ctx)
{
    /* Stereo 48 kHz is supported by the capture miniport. Require a
     * matching, nonempty ACPI blob for the exact DAI format. */
    static const USHORT samples[][2] = {{16, 16}, {32, 24}, {32, 32}};
    SOF_NHLT_CONFIG_KEY key = {
        NHLT_LINK_DMIC, 0, NHLT_DIRECTION_CAPTURE, NHLT_DEVICE_DMIC, 48000, 2, 0, 0
    };
    if (!Ctx->NhltTable) return STATUS_NOT_FOUND;
    for (ULONG i = 0; i < RTL_NUMBER_OF(samples); ++i) {
        ULONG bytes;
        key.ContainerBits = samples[i][0];
        key.ValidBits = samples[i][1];
        NTSTATUS status = SofNhltCopyConfiguration(Ctx->NhltTable, Ctx->NhltTableSize,
                                                   &key, NULL, 0, &bytes);
        if (status == STATUS_BUFFER_TOO_SMALL) {
            if ((bytes & 3u) || bytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD -
                    (sizeof(struct sof_ipc4_copier_data) - sizeof(ULONG)))
                return STATUS_INVALID_BUFFER_SIZE;
            pj_firmware_config record;
            ULONG copierPages, gainPages;
            ULONG chunkBytes = key.SampleRate / 1000u * key.Channels * (key.ContainerBits / 8u);
            if (pj_firmware_plan_ll(&Ctx->CopierProfile, chunkBytes, chunkBytes,
                    &record, &copierPages) != PJ_CATALOG_OK ||
                pj_firmware_plan_ll(&Ctx->GainProfile, chunkBytes, chunkBytes,
                    &record, &gainPages) != PJ_CATALOG_OK ||
                2u * copierPages + gainPages > 20u)
                continue;
            Ctx->AudioFormat.Rate = key.SampleRate;
            Ctx->AudioFormat.Channels = key.Channels;
            Ctx->AudioFormat.ContainerBits = key.ContainerBits;
            Ctx->AudioFormat.ValidBits = key.ValidBits;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_NOT_FOUND) return status;
    }
    return STATUS_NOT_FOUND;
}
#endif

NTSTATUS SofPublishAudioChild(PSOF_DEVICE_CONTEXT Ctx)
{
#ifndef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_NOT_SUPPORTED;
#else
    DECLARE_CONST_UNICODE_STRING(hardwareId, PUJJO_AUDIO_CHILD_ID);
    DECLARE_CONST_UNICODE_STRING(instanceId, L"DMIC");
    PWDFDEVICE_INIT init;
    WDFDEVICE child;
    WDF_QUERY_INTERFACE_CONFIG config;
    PUJJO_AUDIO_BUS bus = {0};
    NTSTATUS status;

    if (Ctx->AudioChild || Ctx->AudioDmaFaulted || !Ctx->DmaEnabler ||
        Ctx->FwState != SOF_FW_BOOT_COMPLETE || !Ctx->NumCaptureStreams)
        return STATUS_DEVICE_NOT_READY;
    status = AudioSelectFormat(Ctx);
    if (!NT_SUCCESS(status)) return status;

    init = WdfPdoInitAllocate(Ctx->WdfDevice);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    status = WdfPdoInitAssignDeviceID(init, &hardwareId);
    if (NT_SUCCESS(status)) status = WdfPdoInitAddHardwareID(init, &hardwareId);
    if (NT_SUCCESS(status)) status = WdfPdoInitAssignInstanceID(init, &instanceId);
    if (NT_SUCCESS(status)) status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    bus.Header.Size = (USHORT)sizeof(bus);
    bus.Header.Version = PUJJO_AUDIO_INTERFACE_VERSION;
    bus.Header.Context = Ctx;
    bus.Header.InterfaceReference = AudioReference;
    bus.Header.InterfaceDereference = AudioDereference;
    bus.GetFormat = AudioGetFormat;
    bus.Allocate = AudioAllocate;
    bus.Free = AudioFree;
    bus.SetState = AudioSetState;
    bus.Position = AudioPosition;
    bus.Notification = AudioNotification;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&config, &bus.Header, &GUID_PUJJO_AUDIO_BUS, NULL);
    status = WdfDeviceAddQueryInterface(child, &config);
    if (NT_SUCCESS(status)) {
        AudioLock(Ctx);
        AudioSetOnline(Ctx, TRUE);
        AudioUnlock(Ctx);
        status = WdfFdoAddStaticChild(Ctx->WdfDevice, child);
    }
    if (!NT_SUCCESS(status)) {
        SofDisconnectAudioChild(Ctx);
        WdfObjectDelete(child);
        return status;
    }
    Ctx->AudioChild = child;
    return STATUS_SUCCESS;
#endif
}
