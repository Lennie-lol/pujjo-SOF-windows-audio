/* SPDX-License-Identifier: BSD-3-Clause */
#include <initguid.h>
#include "PujjoAudioBus.h"
#include "Device.h"
#include "WaveRt.h"
#include "PujjoDmaPlan.h"
#include "PujjoIpc4Protocol.h"
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
#include "PujjoAnalog.h"
#endif

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

/* V3-style allocation is not accepted for a real DMIC child. The caller
 * must use the v5 CPU-page lease, so older children cannot map DMA pages. */
static NTSTATUS AudioAllocate(PVOID Context, ULONG RequestedBytes, PUJJO_AUDIO_BUFFER *Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(RequestedBytes);
    if (Buffer) RtlZeroMemory(Buffer, sizeof(*Buffer));
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS AudioPlanVoice(PVOID Context, ULONG RequestedBytes, PULONG ActualBytes)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    ULONG size, periodMs;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (!ActualBytes) return STATUS_INVALID_PARAMETER;
    *ActualBytes = 0;
    AudioLock(ctx);
    if (ctx->AudioOnline && !ctx->AudioDmaFaulted && !SofDeviceRemoved(ctx)) {
        if (!pj_dmic_voice_ring(RequestedBytes, &size, &periodMs))
            status = STATUS_INVALID_BUFFER_SIZE;
        else {
            *ActualBytes = size / 8u;
            status = STATUS_SUCCESS;
        }
    }
    AudioUnlock(ctx);
    return status;
}

static NTSTATUS AudioAttachVoice(PVOID Context, ULONG RequestedBytes, PVOID VoiceAddress,
                                 ULONG ActualBytes, PVOID *Lease)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    PSOF_WAVERT_STREAM stream;
    SOF_AUDIO_FORMAT_PARAMS format;
    ULONG size, periodMs;
    NTSTATUS status;
    if (!Lease) return STATUS_INVALID_PARAMETER;
    *Lease = NULL;
    if (!VoiceAddress || BYTE_OFFSET(VoiceAddress) ||
        !pj_dmic_voice_ring(RequestedBytes, &size, &periodMs) || ActualBytes != size / 8u)
        return STATUS_INVALID_BUFFER_SIZE;
    AudioLock(ctx);
    if (!ctx->AudioOnline || ctx->AudioDmaFaulted || SofDeviceRemoved(ctx)) {
        status = STATUS_DEVICE_NOT_READY;
        goto Done;
    }
    if (ctx->DmicStream) {
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }

    /* The firmware carries four signed 32-bit slots; the CPU delivers
     * one signed 16-bit voice sample per frame to the port-owned ring. */
    SofIpc4BuildDefaultStereoFormat(PJ_DMIC_RATE, PJ_DMIC_BITS, &format);
    format.ChannelsCount = PJ_DMIC_CHANNELS;
    format.ChannelMap = SOF_IPC4_CH_MAP_QUAD;
    format.ChannelConfig = SOF_IPC4_CHANNEL_CONFIG_3_POINT_1;
    format.ValidBitDepth = PJ_DMIC_BITS;
    status = SofWaveRtCreateStream(ctx, SOF_ENDPOINT_DMIC_PDM, TRUE, 0, &format, size,
                                   periodMs, &stream);
    if (!NT_SUCCESS(status)) goto Done;
    LARGE_INTEGER frequency;
    KeQueryPerformanceCounter(&frequency);
    stream->CounterFrequency = (ULONGLONG)frequency.QuadPart;
    /* Scratch is private kernel memory, separate from every mapped page. */
    stream->VoiceScratch = ExAllocatePool2(POOL_FLAG_NON_PAGED, ActualBytes / 2u, SOF_DEVICE_TAG);
    if (!stream->VoiceScratch) {
        if (!NT_SUCCESS(SofWaveRtFreeStream(stream))) ctx->AudioDmaFaulted = TRUE;
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    stream->VoiceBuffer = (SHORT *)VoiceAddress;
    RtlZeroMemory(VoiceAddress, ActualBytes);
    ctx->DmicStream = stream;
    *Lease = stream;
    AudioSetPositionEnabled(ctx, TRUE);
Done:
    AudioUnlock(ctx);
    return status;
}

/* Cleanup is allowed after a device-wide fault. Preserve the first error
 * and still stop DMA when the gain command fails. */
static NTSTATUS AudioStopCapture(PSOF_DEVICE_CONTEXT ctx,
                                 PSOF_WAVERT_STREAM stream, KSSTATE state)
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS transition;
    if (stream->State == KSSTATE_RUN)
        status = SofWaveRtSetMute(stream, TRUE);
    transition = SofWaveRtSetState(stream, state);
    if (NT_SUCCESS(status)) status = transition;
    if (!NT_SUCCESS(status)) ctx->AudioDmaFaulted = TRUE;
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
        if (!stream->Detached)
            (void)AudioStopCapture(ctx, stream, KSSTATE_STOP);
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
        if (State != KSSTATE_RUN) {
            status = AudioStopCapture(ctx, stream, (KSSTATE)State);
        } else {
            status = STATUS_SUCCESS;
            if (stream->State != KSSTATE_RUN)
                status = SofWaveRtSetMute(stream, TRUE);
            if (NT_SUCCESS(status))
                status = SofWaveRtSetState(stream, KSSTATE_RUN);
            if (NT_SUCCESS(status))
                status = SofWaveRtSetMute(stream, FALSE);
            if (!NT_SUCCESS(status))
                (void)AudioStopCapture(ctx, stream, KSSTATE_STOP);
        }
        if (NT_SUCCESS(status) && !ctx->AudioDmaFaulted)
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
    if (!Stream || !Position) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&ctx->AudioPositionLock, &irql);
    /* Allocation publishes the pointer before enabling reads; Free disables
     * and drains this lock before clearing the pointer and freeing memory. */
    if (ctx->AudioOnline && ctx->AudioPositionEnabled && ctx->DmicStream == Stream) {
        PSOF_WAVERT_STREAM stream = Stream;
        if (SofDeviceRemoved(ctx) || InterlockedCompareExchange(&stream->DmaFaulted, 0, 0))
            status = STATUS_DEVICE_NOT_READY;
        else {
            *Position = (ULONG)InterlockedCompareExchange(&stream->VoicePosition, 0, 0);
            status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&ctx->AudioPositionLock, irql);
    return status;
}

static NTSTATUS AudioReadPacket(PVOID Context, PVOID Stream, PULONG Number,
                                PULONGLONG Counter, PBOOLEAN More)
{
    PSOF_DEVICE_CONTEXT ctx = Context;
    PSOF_WAVERT_STREAM stream = Stream;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    KIRQL irql;
    if (!Number || !Counter || !More) return STATUS_INVALID_PARAMETER;
    AudioLock(ctx);
    if (ctx->AudioOnline && ctx->DmicStream == stream && stream &&
        !SofDeviceRemoved(ctx) && InterlockedCompareExchange(&stream->DmaFaulted, 0, 0)) {
        status = STATUS_IO_DEVICE_ERROR;
    } else if (ctx->AudioOnline && ctx->DmicStream == stream && stream &&
        !SofDeviceRemoved(ctx)) {
        KeAcquireSpinLock(&stream->NotificationLock, &irql);
        if (stream->VoiceReadCount < stream->VoicePacketCount) {
            if (stream->VoicePacketCount - stream->VoiceReadCount > 2u)
                stream->VoiceReadCount = stream->VoicePacketCount - 2u;
            *Number = (ULONG)stream->VoiceReadCount;
            *Counter = stream->VoiceTimestamp[stream->VoiceReadCount % 2u];
            *More = ++stream->VoiceReadCount < stream->VoicePacketCount;
            status = STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&stream->NotificationLock, irql);
    }
    AudioUnlock(ctx);
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
    if (Ctx->DmicStream) {
        PSOF_WAVERT_STREAM stream = Ctx->DmicStream;
        /* Mute a live capture route before detaching its DMA resources. */
        if (!stream->Detached && stream->State == KSSTATE_RUN)
            status = SofWaveRtSetMute(stream, TRUE);
        NTSTATUS detach = SofWaveRtDetachStream(stream);
        if (NT_SUCCESS(status)) status = detach;
        if (!NT_SUCCESS(status)) Ctx->AudioDmaFaulted = TRUE;
    }
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
    NTSTATUS analogStatus = SofDisconnectAnalogLocked(Ctx);
    if (NT_SUCCESS(status)) status = analogStatus;
#endif
    AudioUnlock(Ctx);
    return status;
}

#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
static NTSTATUS AudioSelectFormat(PSOF_DEVICE_CONTEXT Ctx)
{
    pj_dmic_profile profile;
    pj_firmware_config record;
    PJ_DWORD copierPages, gainPages;
    NTSTATUS status;
    if (!Ctx) return STATUS_INVALID_PARAMETER;
    if (!Ctx->NhltTable || !Ctx->NhltTableSize) return STATUS_NOT_FOUND;
    /* Reviewed four-slot PCM99 only (native or explicitly scoped DMIC candidate).
     * Never publish an arbitrary stereo route
     * just because firmware happens to advertise some other DMIC format. */
    status = SofNhltInspectPujjoDmic(Ctx->NhltTable, Ctx->NhltTableSize, &profile);
    if (!NT_SUCCESS(status)) return status;
    if (profile.bytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD -
            (sizeof(struct sof_ipc4_copier_data) - sizeof(ULONG)))
        return STATUS_INVALID_BUFFER_SIZE;
    if (pj_firmware_plan_ll(&Ctx->CopierProfile, PJ_DMIC_CHUNK_BYTES, PJ_DMIC_CHUNK_BYTES,
            &record, &copierPages) != PJ_CATALOG_OK ||
        pj_firmware_plan_ll(&Ctx->GainProfile, PJ_DMIC_CHUNK_BYTES, PJ_DMIC_CHUNK_BYTES,
            &record, &gainPages) != PJ_CATALOG_OK ||
        copierPages > 10u || gainPages > 20u - 2u * copierPages)
        return STATUS_NOT_SUPPORTED;
    Ctx->AudioFormat.Rate = PJ_DMIC_RATE;
    Ctx->AudioFormat.Channels = 1;
    Ctx->AudioFormat.ContainerBits = 16;
    Ctx->AudioFormat.ValidBits = 16;
    Ctx->AudioFormat.Role = PujjoAudioDmic;
    return STATUS_SUCCESS;
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
    WDFDEVICE child = NULL;
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
    bus.Header.Version = PUJJO_AUDIO_PACKET_VERSION;
    bus.Header.Context = Ctx;
    bus.Header.InterfaceReference = AudioReference;
    bus.Header.InterfaceDereference = AudioDereference;
    bus.GetFormat = AudioGetFormat;
    bus.Allocate = AudioAllocate;
    bus.Free = AudioFree;
    bus.SetState = AudioSetState;
    bus.Position = AudioPosition;
    bus.Notification = AudioNotification;
    bus.ReadPacket = AudioReadPacket;
    bus.PlanVoice = AudioPlanVoice;
    bus.AttachVoice = AudioAttachVoice;
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
