/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file WaveRt.c
 * @brief PortCls WaveRT Audio Streaming Engine Implementation for Sound Open Firmware.
 *
 * @details
 * Implements the core streaming engine for Windows audio endpoints:
 *  1. Circular Ring Buffer & BDL Setup: Allocates non-paged contiguous physical memory
 *     for audio sample exchange between user-mode (audiosrv) and kernel/hardware. Builds
 *     a 2-entry double-buffered Buffer Descriptor List (BDL) with Interrupt-On-Completion (IOC)
 *     set on both halves to guarantee smooth circular wrapping.
 *  2. Timer & Periodic Event Notifications: Operates a high-resolution kernel timer and
 *     Deferred Procedure Call (DPC) to signal registered KEVENTs at precise period intervals
 *     (typically 10ms = 100Hz), enabling glitch-free audio streaming.
 *  3. BAR0 HD-A Stream Descriptor Programming: Configures hardware stream engines (SD_CTL,
 *     SD_CBL, SD_FORMAT, SD_BDLPL/PU, SD_LVI, SD_STS) and decouples host DMA directly into
 *     the DSP FIFO using Processing Pipe Control (PPCTL).
 *  4. Processing Pipe Link Control (PPLC) & Multi-Link (ML): Programs link DMA engines for
 *     routing DSP audio across external physical interfaces (HD-A codec link, Display Audio /
 *     HDMI multi-link pipes).
 *  5. Dynamic Audio Graph Assembly: Instantiates and binds DSP IPC4 pipelines (Host Copier ->
 *     Gain -> DAI Copier) on demand across diverse endpoints (Analog HD-A, HDMI 1-3, DMIC,
 *     SSP2, SoundWire, and USB Offload).
 *  6. Volume & Mute Controls: Manages volume levels in 1/65536 dB units (clamped to [-96dB, 0dB])
 *     and dispatches IPC4 messages to hardware Gain modules in DSP SRAM.
 * Copyright 2026 The SOF Authors
 */

#include "WaveRt.h"
#include "Device.h"
#include "HdaVerb.h"
#include "PujjoDmaPlan.h"

#ifdef _KERNEL_MODE
#define SOF_HDA_PERIOD_COMPLETE 0x04u

static PUCHAR SofWaveRtHdaDescriptor(PSOF_WAVERT_STREAM Stream)
{
    PSOF_DEVICE_CONTEXT ctx;
    ULONG offset;
    if (!Stream || !Stream->DeviceContext || Stream->StreamIndex >= 30u)
        return NULL;
    ctx = Stream->DeviceContext;
    offset = SOF_HDA_STREAM_REG_OFFSET(Stream->StreamIndex);
    if (SofDeviceRemoved(ctx) || !ctx->Bar0VirtualAddress || offset > ctx->Bar0Length ||
        SOF_HDA_ADSP_SD_ENTRY_SIZE > ctx->Bar0Length - offset)
        return NULL;
    return (PUCHAR)ctx->Bar0VirtualAddress + offset;
}

static NTSTATUS SofWaveRtReserveDescriptor(PSOF_WAVERT_STREAM Stream)
{
    PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
    ULONG descriptorBit = 1u << Stream->StreamIndex;
    ULONG tagBit = 1u << Stream->StreamTag;
    ULONG *tagMask = &ctx->StreamTagMask[Stream->IsCapture ? 1u : 0u];
    KIRQL irql;
    NTSTATUS status = STATUS_DEVICE_BUSY;

    KeAcquireSpinLock(&ctx->IpcLock, &irql);
    if (!(ctx->StreamDescriptorMask & descriptorBit) && !(*tagMask & tagBit)) {
        ctx->StreamDescriptorMask |= descriptorBit;
        *tagMask |= tagBit;
        Stream->DescriptorReserved = TRUE;
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&ctx->IpcLock, irql);
    return status;
}

static void SofWaveRtReleaseDescriptor(PSOF_WAVERT_STREAM Stream)
{
    PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
    KIRQL irql;
    if (!Stream->DescriptorReserved) return;

    KeAcquireSpinLock(&ctx->IpcLock, &irql);
    ctx->StreamDescriptorMask &= ~(1u << Stream->StreamIndex);
    ctx->StreamTagMask[Stream->IsCapture ? 1u : 0u] &= ~(1u << Stream->StreamTag);
    Stream->DescriptorReserved = FALSE;
    KeReleaseSpinLock(&ctx->IpcLock, irql);
}

/* Copy only the inactive DMA half. A wrap during conversion invalidates
 * the packet. The cursor and event are published only after revalidation. */
static BOOLEAN SofWaveRtCopyVoicePeriod(PSOF_WAVERT_STREAM Stream)
{
    ULONG before, after;
    ULONG period = Stream->PeriodSizeBytes;
    ULONG offset = (ULONG)InterlockedCompareExchange(&Stream->VoicePosition, 0, 0) * 8u;
    ULONGLONG counter;
    ULONGLONG started = KeQueryInterruptTimePrecise(&counter);
    if (started - Stream->VoiceLastCompletion >= (ULONGLONG)Stream->PeriodMs * 20000u ||
        !NT_SUCCESS(SofWaveRtGetPosition(Stream, &before)) ||
        (before < period) == (offset < period))
        return FALSE;
    /* DPIB counts bytes into the current DMA half. Backdate its observation
     * by that progress plus one full period to timestamp the first frame. */
    ULONGLONG frames = (period + before % period) / 16u;
    ULONGLONG ticks = frames * Stream->CounterFrequency / PJ_DMIC_RATE;
    if (counter < ticks) return FALSE;
    ULONGLONG packetStart = counter - ticks;
    const LONG *slots = (const LONG *)((PUCHAR)Stream->KernelBuffer + offset);
    SHORT *scratch = (SHORT *)((PUCHAR)Stream->VoiceBuffer +
        ROUND_TO_PAGES(Stream->BufferSizeBytes / 8u));
    for (ULONG i = 0; i < period / 16u; ++i, slots += 4) {
        LONGLONG sum = (LONGLONG)slots[0] + slots[1] + slots[2] + slots[3];
        scratch[i] = (SHORT)(sum / (4 * 65536));
    }
    KeMemoryBarrier();
    if (!NT_SUCCESS(SofWaveRtGetPosition(Stream, &after)) ||
        (after < period) != (before < period) || after < before ||
        KeQueryInterruptTimePrecise(&counter) - started >= (ULONGLONG)Stream->PeriodMs * 10000u)
        return FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&Stream->NotificationLock, &irql);
    RtlCopyMemory(Stream->VoiceBuffer + offset / 16u, scratch, period / 8u);
    Stream->VoiceTimestamp[Stream->VoicePacketCount % 2u] = packetStart;
    ++Stream->VoicePacketCount;
    Stream->VoiceLastCompletion = started;
    InterlockedExchange(&Stream->VoicePosition, offset ? 0 : (LONG)(period / 8u));
    KeReleaseSpinLock(&Stream->NotificationLock, irql);
    return TRUE;
}

static KDEFERRED_ROUTINE SofWaveRtTimerDpc;

/**
 * @brief Periodic timer DPC callback routine.
 *
 * Polls the HDA stream status and signals events on a hardware completion.
 *
 * @param[in]     Dpc             Pointer to DPC object.
 * @param[in,opt] DeferredContext Context pointer (SOF_WAVERT_STREAM).
 * @param[in,opt] SystemArgument1 Unused system argument.
 * @param[in,opt] SystemArgument2 Unused system argument.
 */
static VOID SofWaveRtTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PSOF_WAVERT_STREAM stream = (PSOF_WAVERT_STREAM)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (stream != NULL && stream->State == KSSTATE_RUN &&
        !InterlockedCompareExchange(&stream->DmaFaulted, 0, 0)) {
        if (InterlockedCompareExchange(&stream->VoiceCopyBusy, 1, 0)) return;
        PUCHAR regs = SofWaveRtHdaDescriptor(stream);
        UCHAR flags = regs ? READ_REGISTER_UCHAR(regs + SOF_HDA_ADSP_REG_SD_STS) : 0xffu;
        if (flags == 0xffu || (flags & SOF_HDA_SD_STS_DMA_ERRORS)) {
            InterlockedExchange(&stream->DmaFaulted, TRUE);
        } else if (flags & SOF_HDA_PERIOD_COMPLETE) {
            WRITE_REGISTER_UCHAR(regs + SOF_HDA_ADSP_REG_SD_STS, SOF_HDA_PERIOD_COMPLETE);
            if (!stream->VoiceBuffer || SofWaveRtCopyVoicePeriod(stream))
                SofWaveRtSignalNotification(stream);
            else
                InterlockedExchange(&stream->DmaFaulted, TRUE);
        }
        InterlockedExchange(&stream->VoiceCopyBusy, 0);
    }
}
#endif

/**
 * @brief Allocates and initializes a new WaveRT audio stream.
 *
 * Performs:
 *  1. Stream descriptor allocation and endpoint-to-pipeline mapping.
 *  2. Cyclic audio buffer and 2-entry BDL physical memory allocation.
 *  3. On-demand DSP IPC4 audio pipeline creation (Host Copier -> Gain -> DAI Copier).
 *  4. BAR0 HD-A stream engine programming and decoupling via PPCTL.
 *  5. Link DMA (PPLC) configuration for codec and display audio links.
 *
 * @param[in]  Ctx             Pointer to SOF device context.
 * @param[in]  EndpointType    Target endpoint type (e.g. HDA_ANALOG, HDMI_1, DMIC_PDM).
 * @param[in]  IsCapture       TRUE for recording; FALSE for playback.
 * @param[in]  StreamTag       Preferred HD-A stream tag (1..15); 0 for default.
 * @param[in]  Format          Audio format specification.
 * @param[in]  BufferSizeBytes Requested circular buffer size in bytes.
 * @param[in]  PeriodMs        Period duration in milliseconds.
 * @param[out] StreamOut       Receives allocated SOF_WAVERT_STREAM pointer.
 * @return STATUS_SUCCESS on success, or an NTSTATUS failure code.
 */
/* Cached controls are published only after acknowledged gain commands.
 * Two-channel independent levels are supported; other formats use master gain.
 * Caller serializes stream controls and lifetime. No hardware gate is lifted. */
static NTSTATUS SofWaveRtApplyCachedGain(PSOF_WAVERT_STREAM Stream, BOOLEAN Mute)
{
    NTSTATUS status;
    if (!Stream || !Stream->DeviceContext || Stream->GainInstance == 0xFFFFFFFFu)
        return STATUS_DEVICE_NOT_READY;
    if (Mute)
        return SofIpc4SetMute(Stream->DeviceContext, Stream->GainInstance,
            SOF_IPC4_GAIN_ALL_CHANNELS_MASK, TRUE, 0);
    if (Stream->Format.ChannelsCount > 2u || Stream->VolumeLevelDb[0] == Stream->VolumeLevelDb[1])
        return SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance,
            SOF_IPC4_GAIN_ALL_CHANNELS_MASK, Stream->VolumeLevelDb[0]);
    if (Stream->Format.ChannelsCount == 1u)
        return SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance,
            SOF_IPC4_GAIN_ALL_CHANNELS_MASK, Stream->VolumeLevelDb[0]);
    if (Stream->Format.ChannelsCount != 2u)
        return STATUS_NOT_SUPPORTED;
    status = SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance,
        0u, Stream->VolumeLevelDb[0]);
    if (NT_SUCCESS(status))
        status = SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance,
            1u, Stream->VolumeLevelDb[1]);
    if (!NT_SUCCESS(status)) {
        /* Best effort only: an IPC fault can prevent even this rollback.
         * Propagate the original error rather than claiming hardware mute. */
        (void)SofIpc4SetMute(Stream->DeviceContext, Stream->GainInstance,
            SOF_IPC4_GAIN_ALL_CHANNELS_MASK, TRUE, 0);
    }
    return status;
}

static NTSTATUS SofWaveRtCreateStreamCore(
    _In_ PSOF_DEVICE_CONTEXT Ctx,
    _In_ SOF_ENDPOINT_TYPE EndpointType,
    _In_ BOOLEAN IsCapture,
    _In_ ULONG StreamTag,
    _In_ const SOF_AUDIO_FORMAT_PARAMS *Format,
    _In_ ULONG BufferSizeBytes,
    _In_ ULONG PeriodMs,
    _In_ ULONG VerifiedSspPort,
    _Out_ PSOF_WAVERT_STREAM *StreamOut
)
{
    PSOF_WAVERT_STREAM stream;
    NTSTATUS status;
#ifndef _KERNEL_MODE
    ULONG bdlSize;
#endif
    ULONG periodBytes;
#ifdef _KERNEL_MODE
    ULONG directionSlot, descriptorIndex;
#endif

    if (StreamOut == NULL) return STATUS_INVALID_PARAMETER;
    *StreamOut = NULL;
    if (Format == NULL || (ULONG)EndpointType >= SOF_ENDPOINT_COUNT ||
        StreamTag > 15u || Format->InterleavingStyle != 0u ||
        !pj_pcm_period(Format->SamplingFrequency, Format->ChannelsCount,
                       Format->BitDepth, Format->ValidBitDepth,
                       PeriodMs ? PeriodMs : SOF_WAVERT_DEFAULT_PERIOD_MS,
                       BufferSizeBytes, &periodBytes))
        return STATUS_INVALID_PARAMETER;
#ifdef _KERNEL_MODE
#ifndef PUJJO_ENABLE_EXPERIMENTAL_AUDIO
    return STATUS_NOT_SUPPORTED;
#endif
    /* Creation is serialized by the parent audio bus. */
    if (Ctx == NULL || Ctx->AudioDmaFaulted || Ctx->FwState != SOF_FW_BOOT_COMPLETE)
        return STATUS_DEVICE_NOT_READY;
    if (EndpointType == SOF_ENDPOINT_DMIC_PDM) {
        if (!Ctx->AudioOnline) return STATUS_DEVICE_NOT_READY;
        if (!IsCapture) return STATUS_INVALID_PARAMETER;
        if (Format->SamplingFrequency != PJ_DMIC_RATE ||
            Format->ChannelsCount != PJ_DMIC_CHANNELS || Format->BitDepth != PJ_DMIC_BITS ||
            Format->ValidBitDepth != PJ_DMIC_BITS)
            return STATUS_NOT_SUPPORTED;
        directionSlot = 0;
    }
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
    else if (EndpointType == SOF_ENDPOINT_PUJJO_HEADPHONE && !IsCapture && VerifiedSspPort == 0) {
        directionSlot = 0;
    } else if (EndpointType == SOF_ENDPOINT_PUJJO_HEADSET_MIC && IsCapture && VerifiedSspPort == 0) {
        directionSlot = 1;
    } else if (EndpointType == SOF_ENDPOINT_PUJJO_SPEAKER && !IsCapture && VerifiedSspPort == 1) {
        directionSlot = 1;
    }
#endif
    else {
        return STATUS_NOT_SUPPORTED;
    }
    if (Format->SamplingFrequency != 48000u ||
        (Format->BitDepth != 16u && Format->BitDepth != 32u) ||
        (Format->ValidBitDepth != 16u && Format->ValidBitDepth != 24u &&
         Format->ValidBitDepth != 32u) ||
        (Format->BitDepth == 32u && Format->ValidBitDepth == 16u))
        return STATUS_NOT_SUPPORTED;
    if (Format->ChannelsCount == 4u &&
        (EndpointType != SOF_ENDPOINT_DMIC_PDM || Format->BitDepth != 32u ||
         Format->ValidBitDepth != 32u || Format->ChannelMap != SOF_IPC4_CH_MAP_QUAD ||
         Format->ChannelConfig != SOF_IPC4_CHANNEL_CONFIG_3_POINT_1))
        return STATUS_NOT_SUPPORTED;
    if (Format->ChannelsCount != 2u && Format->ChannelsCount != 4u)
        return STATUS_NOT_SUPPORTED;
    if (StreamTag && StreamTag != directionSlot + 1u)
        return STATUS_INVALID_PARAMETER;
    if (!Ctx->Bar0VirtualAddress || !pj_hda_stream_slot(Ctx->NumCaptureStreams,
        Ctx->NumPlaybackStreams, IsCapture ? 1u : 0u, directionSlot,
        Ctx->Bar0Length, &descriptorIndex)) return STATUS_DEVICE_CONFIGURATION_ERROR;
#endif

#ifdef _KERNEL_MODE
    stream = (PSOF_WAVERT_STREAM)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SOF_WAVERT_STREAM), SOF_DEVICE_TAG);
#else
    stream = (PSOF_WAVERT_STREAM)calloc(1, sizeof(SOF_WAVERT_STREAM));
#endif
    if (stream == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(stream, sizeof(SOF_WAVERT_STREAM));
    stream->DeviceContext = Ctx;
    stream->EndpointType = EndpointType;
    stream->IsCapture = IsCapture;

    /* Assign pipeline IDs, stream tags, and hardware stream descriptor indices */
    if (IsCapture) {
        if (EndpointType == SOF_ENDPOINT_DMIC_PDM) {
            stream->PipelineId = SOF_PIPELINE_ID_DMIC;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_DMIC;
            stream->StreamIndex = 1;
        } else if (EndpointType == SOF_ENDPOINT_I2S_SSP2) {
            stream->PipelineId = SOF_PIPELINE_ID_SSP_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SSP_CAPTURE;
            stream->StreamIndex = 2;
        } else if (EndpointType == SOF_ENDPOINT_SDW_CAPTURE) {
            stream->PipelineId = SOF_PIPELINE_ID_SDW_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SDW_CAPTURE;
            stream->StreamIndex = 3;
        } else {
            stream->PipelineId = SOF_PIPELINE_ID_CAPTURE;
            stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_CAPTURE;
            stream->StreamIndex = 0;
        }
    } else if (EndpointType == SOF_ENDPOINT_HDMI_1) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_1;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_1;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 1 : 8;
    } else if (EndpointType == SOF_ENDPOINT_HDMI_2) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_2;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_2;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 2 : 9;
    } else if (EndpointType == SOF_ENDPOINT_HDMI_3) {
        stream->PipelineId = SOF_PIPELINE_ID_HDMI_3;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_HDMI_3;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 3 : 10;
    } else if (EndpointType == SOF_ENDPOINT_I2S_SSP2) {
        stream->PipelineId = SOF_PIPELINE_ID_SSP_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SSP_RENDER;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 4 : 11;
    } else if (EndpointType == SOF_ENDPOINT_SDW_RENDER) {
        stream->PipelineId = SOF_PIPELINE_ID_SDW_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_SDW_RENDER;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams + 5 : 12;
    } else {
        stream->PipelineId = SOF_PIPELINE_ID_PLAYBACK;
        stream->StreamTag = (StreamTag > 0) ? StreamTag : SOF_WAVERT_STREAM_TAG_PLAYBACK;
        stream->StreamIndex = (Ctx != NULL && Ctx->NumCaptureStreams > 0) ? Ctx->NumCaptureStreams : 7;
    }

#ifdef _KERNEL_MODE
    /* Override the legacy host-simulation descriptor choices with the
     * actual bounded capability-derived descriptor. */
    stream->StreamIndex = descriptorIndex;
    stream->StreamTag = directionSlot + 1u;
#ifdef PUJJO_ENABLE_EXPERIMENTAL_ANALOG
    switch (EndpointType) {
    case SOF_ENDPOINT_PUJJO_HEADPHONE: stream->PipelineId = SOF_PIPELINE_ID_PUJJO_HEADPHONE; break;
    case SOF_ENDPOINT_PUJJO_HEADSET_MIC: stream->PipelineId = SOF_PIPELINE_ID_PUJJO_HEADSET_MIC; break;
    case SOF_ENDPOINT_PUJJO_SPEAKER: stream->PipelineId = SOF_PIPELINE_ID_PUJJO_SPEAKER; break;
    default: break;
    }
#endif
#endif
    stream->Format = *Format;
    stream->BufferSizeBytes = BufferSizeBytes;
    stream->PeriodMs = (PeriodMs > 0) ? PeriodMs : SOF_WAVERT_DEFAULT_PERIOD_MS;
    stream->State = KSSTATE_STOP;
    stream->HostCopierInstance = 0xFFFFFFFF;
    stream->GainInstance = 0xFFFFFFFF;
    stream->DaiCopierInstance = 0xFFFFFFFF;

    /* Calculate period size in bytes: (Rate * Ch * BytesPerSample * PeriodMs) / 1000 */
    stream->PeriodSizeBytes = periodBytes;

    /* 1. Allocate circular ring buffer */
#ifdef _KERNEL_MODE
    {
        status = SofWaveRtReserveDescriptor(stream);
        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(stream, SOF_DEVICE_TAG);
            return status;
        }
        /* PortCls can map whole pages. Keep every mapped byte within the
         * zeroed ring allocation, including the padding of its last page. */
        status = SofAllocateDmaBuffer(Ctx, (ULONG)ROUND_TO_PAGES(BufferSizeBytes),
                                      &stream->RingAllocation);
        if (NT_SUCCESS(status))
            status = SofAllocateDmaBuffer(Ctx, PAGE_SIZE, &stream->BdlAllocation);
        if (NT_SUCCESS(status)) {
            stream->KernelBuffer = stream->RingAllocation.VirtualAddress;
            stream->PhysicalAddress = stream->RingAllocation.PhysicalAddress;
            stream->Bdl = (PSOF_INTEL_DSP_BDL)stream->BdlAllocation.VirtualAddress;
            stream->BdlPhysicalAddress = stream->BdlAllocation.PhysicalAddress;
            if ((EndpointType != SOF_ENDPOINT_DMIC_PDM && BYTE_OFFSET(stream->KernelBuffer)) ||
                (stream->BdlPhysicalAddress.LowPart & 127u) ||
                !pj_dma_period_count(stream->PhysicalAddress.LowPart,
                                     (ULONG)stream->PhysicalAddress.HighPart,
                                     BufferSizeBytes, periodBytes, &stream->BdlPeriodCount))
                status = STATUS_INVALID_BUFFER_SIZE;
        }
        if (NT_SUCCESS(status)) {
            for (ULONG i = 0; i < stream->BdlPeriodCount; ++i) {
                pj_dma_segment segment;
                if (!pj_dma_period_segment(stream->PhysicalAddress.LowPart,
                        (ULONG)stream->PhysicalAddress.HighPart, BufferSizeBytes,
                        periodBytes, i, &segment)) {
                    status = STATUS_INVALID_BUFFER_SIZE;
                    break;
                }
                stream->Bdl[i].AddrLow = segment.low;
                stream->Bdl[i].AddrHigh = segment.high;
                stream->Bdl[i].Size = segment.length;
                stream->Bdl[i].Ioc = 1;
            }
        }
        if (!NT_SUCCESS(status)) {
            SofFreeDmaBuffer(Ctx, &stream->BdlAllocation);
            SofFreeDmaBuffer(Ctx, &stream->RingAllocation);
            SofWaveRtReleaseDescriptor(stream);
            ExFreePoolWithTag(stream, SOF_DEVICE_TAG);
            return status;
        }

        /* Initialize timer and DPC for period notifications */
        KeInitializeSpinLock(&stream->NotificationLock);
        KeInitializeDpc(&stream->NotificationDpc, SofWaveRtTimerDpc, stream);
        KeInitializeTimer(&stream->NotificationTimer);
    }
#else
    stream->KernelBuffer = calloc(1, stream->BufferSizeBytes);
    bdlSize = sizeof(SOF_INTEL_DSP_BDL) * 2;
    stream->Bdl = (PSOF_INTEL_DSP_BDL)calloc(1, bdlSize);
    if (stream->KernelBuffer == NULL || stream->Bdl == NULL) {
        if (stream->KernelBuffer) free(stream->KernelBuffer);
        if (stream->Bdl) free(stream->Bdl);
        free(stream);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->PhysicalAddress.QuadPart = 0x10000000ULL;
    stream->BdlPhysicalAddress.QuadPart = 0x20000000ULL;
    stream->Bdl[0].AddrLow = stream->PhysicalAddress.LowPart;
    stream->Bdl[0].Size = stream->BufferSizeBytes / 2;
    stream->Bdl[0].Ioc = 1;
    stream->Bdl[1].AddrLow = stream->PhysicalAddress.LowPart + (stream->BufferSizeBytes / 2);
    stream->Bdl[1].Size = stream->BufferSizeBytes / 2;
    stream->Bdl[1].Ioc = 1;
#endif

    /* Initialize stream volume and mute from device context defaults */
    if (Ctx != NULL && EndpointType < SOF_ENDPOINT_COUNT) {
        stream->VolumeLevelDb[0] = Ctx->DefaultVolumeLevelDb[EndpointType][0];
        stream->VolumeLevelDb[1] = Ctx->DefaultVolumeLevelDb[EndpointType][1];
        stream->MasterMute = TRUE;
        stream->IsMuted[0] = stream->MasterMute;
        stream->IsMuted[1] = stream->MasterMute;
    }

    /* 3. Create SOF IPC4 audio pipeline on-demand based on endpoint */
    switch (EndpointType) {
    case SOF_ENDPOINT_HDMI_1:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 1, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_HDMI_2:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 2, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_HDMI_3:
        status = SofIpc4CreateHdmiPipeline(Ctx, stream->PipelineId, stream->StreamTag, 3, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        break;
    case SOF_ENDPOINT_DMIC_PDM:
        status = SofIpc4CreateDmicPipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        /* Propagate a failed pipeline; never report silent success. */
        break;
    case SOF_ENDPOINT_PUJJO_HEADPHONE:
    case SOF_ENDPOINT_PUJJO_HEADSET_MIC:
    case SOF_ENDPOINT_PUJJO_SPEAKER:
#if defined(_KERNEL_MODE) && defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG)
        status = SofIpc4CreateSspPipeline(Ctx, stream->PipelineId, stream->StreamTag,
            VerifiedSspPort, IsCapture, Format, &stream->HostCopierInstance,
            &stream->GainInstance, &stream->DaiCopierInstance);
#else
        UNREFERENCED_PARAMETER(VerifiedSspPort);
        status = STATUS_NOT_SUPPORTED;
#endif
        break;
    case SOF_ENDPOINT_I2S_SSP2:
        /* Pujjo safety: RT5682/RT1019 initialization and validated SSP port
         * routing are not yet implemented. Never claim playback succeeded. */
        SofLogBreadcrumb("[PUJJO] SSP audio blocked: codec and port not implemented.\n");
        status = STATUS_NOT_SUPPORTED;
        break;
    case SOF_ENDPOINT_SDW_RENDER:
    case SOF_ENDPOINT_SDW_CAPTURE:
        status = SofIpc4CreateAlhPipeline(Ctx, stream->PipelineId, stream->StreamTag, 0 /* Link 0 */, IsCapture, Format,
                                           &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        /* Propagate a failed pipeline; never report silent success. */
        break;
    case SOF_ENDPOINT_HDA_ANALOG:
    default:
        if (IsCapture) {
            status = SofIpc4CreateCapturePipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                                  &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        } else {
            status = SofIpc4CreatePlaybackPipeline(Ctx, stream->PipelineId, stream->StreamTag, Format,
                                                   &stream->HostCopierInstance, &stream->GainInstance, &stream->DaiCopierInstance);
        }
        break;
    }

    if (!NT_SUCCESS(status)) {
        SofWaveRtFreeStream(stream);
        return status;
    }

    /* Do not configure or publish a stream when requested initial gain failed. */
    if (stream->GainInstance != 0xFFFFFFFFu) {
        status = SofWaveRtApplyCachedGain(stream, stream->MasterMute);
        if (!NT_SUCCESS(status)) {
            SofWaveRtFreeStream(stream);
            return status;
        }
    }

    /* 4. Configure BAR0 HDA Stream Engine (if DSP pipeline instantiated) */
    if (stream->HostCopierInstance != 0xFFFFFFFF) {
        status = SofWaveRtConfigureHdaStream(stream);
        if (!NT_SUCCESS(status)) {
            SofWaveRtFreeStream(stream);
            return status;
        }

        /* 5. Configure Link Stream Engine (PPLC) */
        SofWaveRtConfigureLinkStream(stream);
    }

    stream->IsActive = TRUE;
    stream->State = KSSTATE_ACQUIRE;
    *StreamOut = stream;
    return STATUS_SUCCESS;
}

NTSTATUS SofWaveRtCreateStream(PSOF_DEVICE_CONTEXT Ctx, SOF_ENDPOINT_TYPE Endpoint,
    BOOLEAN IsCapture, ULONG StreamTag, const PSOF_AUDIO_FORMAT_PARAMS Format,
    ULONG BufferBytes, ULONG PeriodMs, PSOF_WAVERT_STREAM *Stream)
{
    return SofWaveRtCreateStreamCore(Ctx, Endpoint, IsCapture, StreamTag, Format,
                                      BufferBytes, PeriodMs, 0xffffffffu, Stream);
}

#if defined(_KERNEL_MODE) && defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG)
NTSTATUS SofWaveRtCreateSspStream(PSOF_DEVICE_CONTEXT Ctx, SOF_ENDPOINT_TYPE Endpoint,
    ULONG VerifiedPort, const SOF_AUDIO_FORMAT_PARAMS *Format, ULONG BufferBytes,
    ULONG PeriodMs, PSOF_WAVERT_STREAM *Stream)
{
    if (Endpoint != SOF_ENDPOINT_PUJJO_HEADPHONE && Endpoint != SOF_ENDPOINT_PUJJO_HEADSET_MIC &&
        Endpoint != SOF_ENDPOINT_PUJJO_SPEAKER)
        return STATUS_INVALID_PARAMETER;
    return SofWaveRtCreateStreamCore(Ctx, Endpoint, Endpoint == SOF_ENDPOINT_PUJJO_HEADSET_MIC,
        0, Format, BufferBytes, PeriodMs, VerifiedPort, Stream);
}
#endif

/**
 * @brief Teardown and deallocation of a WaveRT stream instance.
 *
 * Transitions stream to KSSTATE_STOP, caches active volume/mute state in device context,
 * deletes the DSP pipeline and its internal routes, cancels timers, and frees memory.
 *
 * @param[in] Stream Pointer to stream instance to free.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtFreeStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS deleteStatus = STATUS_SUCCESS;
    if (Stream == NULL) {
        return STATUS_SUCCESS;
    }

#ifdef _KERNEL_MODE
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
#endif
    /* Stop stream and reset pipeline. Cancel regardless of registered events
     * and drain queued/running notification DPCs before freeing their context. */
#ifdef _KERNEL_MODE
    if (!Stream->Detached)
#endif
        status = SofWaveRtSetState(Stream, KSSTATE_STOP);
    Stream->IsActive = FALSE;
#ifdef _KERNEL_MODE
    KeCancelTimer(&Stream->NotificationTimer);
    KeRemoveQueueDpc(&Stream->NotificationDpc);
    KeFlushQueuedDpcs();
#endif

    /* Cache stream volume and mute back into device context */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][0] = Stream->VolumeLevelDb[0];
        Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][1] = Stream->VolumeLevelDb[1];
        Stream->DeviceContext->DefaultMute[Stream->EndpointType] = Stream->MasterMute;
    }

    /* IPC4 deletes all internal routes with the pipeline. Send explicit
     * unbind only for links across pipelines, which this graph has none of. */
    if (Stream->DeviceContext != NULL &&
#ifdef _KERNEL_MODE
        !Stream->Detached &&
#endif
        Stream->HostCopierInstance != 0xFFFFFFFFu) {
        deleteStatus = SofIpc4DeletePipeline(Stream->DeviceContext, Stream->PipelineId);
        /* Keep DSP IDs reserved if deletion failed; firmware may still own them. */
        if (NT_SUCCESS(deleteStatus)) {
            SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_COPIER,
                                  Stream->HostCopierInstance);
            Stream->HostCopierInstance = 0xFFFFFFFFu;
            if (Stream->GainInstance != 0xFFFFFFFFu) {
                SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_GAIN,
                                      Stream->GainInstance);
                Stream->GainInstance = 0xFFFFFFFFu;
            }
            if (Stream->DaiCopierInstance != 0xFFFFFFFFu) {
                SofIpc4FreeInstanceId(Stream->DeviceContext, SOF_IPC4_MOD_ID_COPIER,
                                      Stream->DaiCopierInstance);
                Stream->DaiCopierInstance = 0xFFFFFFFFu;
            }
        }

#ifdef _KERNEL_MODE
        KeStallExecutionProcessor(500);
#endif
    }

#ifdef _KERNEL_MODE
    BOOLEAN stopped = !Stream->DescriptorPublished;
    PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
    WDFCOMMONBUFFER ring = Stream->RingAllocation.CommonBuffer;
    WDFCOMMONBUFFER bdl = Stream->BdlAllocation.CommonBuffer;
    if (!stopped && !Stream->Detached) {
        NTSTATUS resetStatus = SofWaveRtResetHdaStream(Stream);
        stopped = NT_SUCCESS(resetStatus);
        if (NT_SUCCESS(status)) status = resetStatus;
    }
    if (Stream->Mdl) IoFreeMdl(Stream->Mdl);
    if (Stream->VoiceBuffer) ExFreePoolWithTag(Stream->VoiceBuffer, SOF_DEVICE_TAG);
    if (stopped) {
        if (NT_SUCCESS(status) && NT_SUCCESS(deleteStatus) && !Stream->Detached)
            SofWaveRtReleaseDescriptor(Stream);
        if (ctx->HardwarePrepared) {
            SofFreeDmaBuffer(ctx, &Stream->BdlAllocation);
            SofFreeDmaBuffer(ctx, &Stream->RingAllocation);
        }
    } else {
        /* Direct analog leases have separate WDF references. A DMIC lease
         * only maps CPU pages, so its DMA ownership can move independently. */
        if (Stream->EndpointType == SOF_ENDPOINT_DMIC_PDM && Stream->BdlAllocation.CommonBuffer)
            SofRetainDmaPair(ctx, Stream->StreamIndex, &Stream->RingAllocation, &Stream->BdlAllocation);
        ctx->AudioDmaFaulted = TRUE;
        status = STATUS_DEVICE_NOT_READY;
    }
    if (Stream->BuffersLeased) {
        WdfObjectDereference(bdl);
        WdfObjectDereference(ring);
        WdfObjectDereference(ctx->DmaEnabler);
    }

    ExFreePoolWithTag(Stream, SOF_DEVICE_TAG);
#else
    if (Stream->Bdl != NULL) {
        free(Stream->Bdl);
    }
    if (Stream->KernelBuffer != NULL) {
        free(Stream->KernelBuffer);
    }
    free(Stream);
#endif

    return NT_SUCCESS(status) ? deleteStatus : status;
}

#ifdef _KERNEL_MODE
NTSTATUS SofWaveRtDetachStream(PSOF_WAVERT_STREAM Stream)
{
    NTSTATUS status, resetStatus;
    if (Stream->Detached) return STATUS_SUCCESS;

    status = SofWaveRtSetState(Stream, KSSTATE_STOP);
    Stream->IsActive = FALSE;
    KeCancelTimer(&Stream->NotificationTimer);
    KeRemoveQueueDpc(&Stream->NotificationDpc);
    KeFlushQueuedDpcs();
    if (Stream->DescriptorPublished) {
        resetStatus = SofWaveRtResetHdaStream(Stream);
        if (NT_SUCCESS(status)) status = resetStatus;
    }
    if (Stream->DescriptorPublished)
        Stream->DeviceContext->AudioDmaFaulted = TRUE;
    if (Stream->VoiceBuffer && Stream->DescriptorPublished && Stream->BdlAllocation.CommonBuffer)
        SofRetainDmaPair(Stream->DeviceContext, Stream->StreamIndex,
                         &Stream->RingAllocation, &Stream->BdlAllocation);
    if (Stream->VoiceBuffer && !Stream->DescriptorPublished) {
        /* PortCls still owns its CPU pages. DMA has no mapping into them. */
        SofFreeDmaBuffer(Stream->DeviceContext, &Stream->BdlAllocation);
        SofFreeDmaBuffer(Stream->DeviceContext, &Stream->RingAllocation);
    }
    Stream->Detached = TRUE;
    return status;
}
#endif

/**
 * @brief Controls stream state transitions (RUN, PAUSE, STOP, ACQUIRE).
 *
 * Sequence for RUN:
 *  1. Checks and initializes Display Audio Codec (CAD 2) if detected.
 *  2. Starts codec converters (DAC or ADC) via immediate verbs.
 *  3. Starts Link Stream DMA (PPLC) and Host HD-A Stream DMA.
 *  4. Sends SOF_IPC4_PIPE_RUNNING to DSP firmware.
 *  5. Arms high-resolution periodic notification timer.
 *
 * Sequence for PAUSE / STOP:
 *  1. Cancels notification timer.
 *  2. Sends SOF_IPC4_PIPE_PAUSED or SOF_IPC4_PIPE_RESET to DSP firmware.
 *  3. Halts Host DMA and Link DMA engines.
 *  4. Stops codec converters.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] NewState Target KSSTATE.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
#ifdef _KERNEL_MODE
/* A latched DMA error requires stream recreation, even after PAUSE/STOP.
 * Reject detached leases before touching a converter, IPC or MMIO. */
static NTSTATUS SofWaveRtCheckTransition(PSOF_WAVERT_STREAM Stream, KSSTATE NewState)
{
    if (Stream->Detached) return STATUS_DEVICE_NOT_READY;
    if (NewState == KSSTATE_RUN) {
        if (InterlockedCompareExchange(&Stream->DmaFaulted, 0, 0))
            return STATUS_IO_DEVICE_ERROR;
        if (!Stream->DeviceContext || Stream->DeviceContext->AudioDmaFaulted)
            return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}
#endif

NTSTATUS SofWaveRtSetState(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ KSSTATE NewState
)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#ifdef _KERNEL_MODE
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
#endif
#ifdef _KERNEL_MODE
    status = SofWaveRtCheckTransition(Stream, NewState);
    if (!NT_SUCCESS(status)) return status;
#endif
    if (NewState == KSSTATE_RUN && (!Stream->IsActive ||
        Stream->HostCopierInstance == 0xFFFFFFFF || !Stream->DeviceContext))
        return STATUS_DEVICE_NOT_READY;

    if (Stream->State == NewState) {
#ifdef _KERNEL_MODE
        if (NewState == KSSTATE_RUN &&
            InterlockedCompareExchange(&Stream->DmaFaulted, 0, 0))
            return STATUS_IO_DEVICE_ERROR;
#endif
        return STATUS_SUCCESS;
    }

    switch (NewState) {
    case KSSTATE_RUN:
        /* Start Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            status = SofHdaStartConverter(Stream->DeviceContext, 0, Stream->IsCapture, Stream->StreamTag);
            if (!NT_SUCCESS(status)) goto run_failed;
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            status = SofHdaStartDisplayConverter(Stream->DeviceContext, 2, hdmiLink, Stream->StreamTag, &Stream->Format);
            if (!NT_SUCCESS(status)) goto run_failed;
        }

        /* Start Link Stream DMA (PPLC) and HDA Stream Engine */
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            if (Stream->State != KSSTATE_PAUSE) {
                status = SofIpc4SetPipelineState(Stream->DeviceContext,
                    Stream->PipelineId, SOF_IPC4_PIPE_PAUSED);
                if (!NT_SUCCESS(status)) goto run_failed;
            }
            SofWaveRtStartLinkStream(Stream);
#ifdef _KERNEL_MODE
            if (Stream->VoiceBuffer) {
                ULONGLONG counter;
                Stream->VoiceLastCompletion = KeQueryInterruptTimePrecise(&counter);
            }
#endif
            status = SofWaveRtStartHdaStream(Stream);
            if (!NT_SUCCESS(status)) goto run_failed;

            /* Set DSP Pipeline state to RUNNING */
            status = SofIpc4SetPipelineState(Stream->DeviceContext, Stream->PipelineId, SOF_IPC4_PIPE_RUNNING);
            {
                char bmsg[128];
                RtlStringCbPrintfA(bmsg, sizeof(bmsg), "[WaveRt] SetState RUN: status=0x%08x (idx=%u, pipe=%u)\n",
                    status, Stream->StreamIndex, Stream->PipelineId);
                SofLogBreadcrumb(bmsg);
            }
        }

        if (!NT_SUCCESS(status)) goto run_failed;

        /* Publish RUN only after all checked start operations succeed. */
        Stream->State = KSSTATE_RUN;
#ifdef _KERNEL_MODE
        /* Start periodic notification timer */
        {
            LARGE_INTEGER dueTime;
            ULONG pollMs = Stream->VoiceBuffer ? max(1u, Stream->PeriodMs / 4u) : Stream->PeriodMs;
            dueTime.QuadPart = -10000LL * pollMs; /* Relative 100ns intervals */
            KeSetTimerEx(&Stream->NotificationTimer, dueTime, pollMs, &Stream->NotificationDpc);
        }
#endif
        break;

    case KSSTATE_PAUSE:
#ifdef _KERNEL_MODE
        KeCancelTimer(&Stream->NotificationTimer);
        KeRemoveQueueDpc(&Stream->NotificationDpc);
        KeFlushQueuedDpcs();
#endif
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            /* Set DSP Pipeline state to PAUSED */
            status = SofIpc4SetPipelineState(Stream->DeviceContext, Stream->PipelineId, SOF_IPC4_PIPE_PAUSED);

            /* Stop HDA Stream Engine (Host DMA) */
            NTSTATUS dmaStatus = SofWaveRtStopHdaStream(Stream);
            if (NT_SUCCESS(status)) status = dmaStatus;

            /* Stop Link Stream DMA (PPLC) */
            SofWaveRtStopLinkStream(Stream);
        }

        /* Stop Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            SofHdaStopConverter(Stream->DeviceContext, 0, Stream->IsCapture);
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            SofHdaStopDisplayConverter(Stream->DeviceContext, 2, hdmiLink);
        }
        Stream->State = NT_SUCCESS(status) ? KSSTATE_PAUSE : KSSTATE_STOP;
        if (!NT_SUCCESS(status)) Stream->IsActive = FALSE;
        break;

    case KSSTATE_STOP:
    case KSSTATE_ACQUIRE:
#ifdef _KERNEL_MODE
        KeCancelTimer(&Stream->NotificationTimer);
        KeRemoveQueueDpc(&Stream->NotificationDpc);
        KeFlushQueuedDpcs();
#endif
#ifdef _KERNEL_MODE
        InterlockedExchange(&Stream->VoicePosition, 0);
        Stream->VoicePacketCount = 0;
        Stream->VoiceReadCount = 0;
#endif
        Stream->LinearPositionBytes = 0;
        if (Stream->HostCopierInstance != 0xFFFFFFFF) {
            if (Stream->State == KSSTATE_RUN)
                status = SofIpc4SetPipelineState(Stream->DeviceContext,
                    Stream->PipelineId, SOF_IPC4_PIPE_PAUSED);

            /* Stop DMA even when the firmware rejects the state change. */
            NTSTATUS dmaStatus = SofWaveRtResetHdaStream(Stream);
            if (NT_SUCCESS(status)) status = dmaStatus;
            if (NT_SUCCESS(status))
                status = SofIpc4SetPipelineState(Stream->DeviceContext,
                    Stream->PipelineId, SOF_IPC4_PIPE_RESET);

            /* Reset Link Stream DMA (PPLC) */
            SofWaveRtResetLinkStream(Stream);
        }

        /* Stop Codec Converter (DAC or ADC) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG) {
            SofHdaStopConverter(Stream->DeviceContext, 0, Stream->IsCapture);
        } else if (Stream->DeviceContext != NULL && Stream->DeviceContext->IDispCodecPresent && Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
            SofHdaStopDisplayConverter(Stream->DeviceContext, 2, hdmiLink);
        }
        Stream->State = NT_SUCCESS(status) ? NewState : KSSTATE_STOP;
        if (!NT_SUCCESS(status)) Stream->IsActive = FALSE;
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    return status;

run_failed:
    /* Do not arm notifications or report RUN after a failed converter/IPC
     * command. Halt any DMA started in this attempt and require recreation. */
    {
        NTSTATUS stopStatus = SofWaveRtStopHdaStream(Stream);
#ifdef _KERNEL_MODE
        if (!NT_SUCCESS(stopStatus)) Stream->DeviceContext->AudioDmaFaulted = TRUE;
#else
        UNREFERENCED_PARAMETER(stopStatus);
#endif
    }
    SofWaveRtStopLinkStream(Stream);
    if (Stream->DeviceContext->HdaCodecPresent && Stream->EndpointType == SOF_ENDPOINT_HDA_ANALOG)
        (void)SofHdaStopConverter(Stream->DeviceContext, 0, Stream->IsCapture);
    else if (Stream->DeviceContext->IDispCodecPresent &&
             Stream->EndpointType >= SOF_ENDPOINT_HDMI_1 && Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
        ULONG hdmiLink = 1 + (Stream->EndpointType - SOF_ENDPOINT_HDMI_1);
        (void)SofHdaStopDisplayConverter(Stream->DeviceContext, 2, hdmiLink);
    }
    Stream->IsActive = FALSE;
    Stream->State = KSSTATE_STOP;
    return status;
}

/**
 * @brief Returns current stream byte position within the circular buffer.
 *
 * @param[in]  Stream        Pointer to stream instance.
 * @param[out] PositionBytes Receives byte offset (0..BufferSizeBytes-1).
 * @return STATUS_SUCCESS on success; STATUS_INVALID_PARAMETER if invalid args.
 */
NTSTATUS SofWaveRtGetPosition(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONG PositionBytes
)
{
    if (Stream == NULL || PositionBytes == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#ifdef _KERNEL_MODE
    PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
    ULONG offset, position;
    if (InterlockedCompareExchange(&Stream->DmaFaulted, 0, 0))
        return STATUS_IO_DEVICE_ERROR;
    if (!ctx || SofDeviceRemoved(ctx) || !ctx->Bar0VirtualAddress || Stream->StreamIndex >= 30u ||
        !Stream->DescriptorPublished)
        return STATUS_DEVICE_NOT_READY;
    /* ADL-N decoupled host DMA position follows the Intel DPIB register. */
    offset = SOF_HDA_DPIB_BASE + Stream->StreamIndex * SOF_HDA_DPIB_STRIDE;
    if (offset > ctx->Bar0Length || sizeof(ULONG) > ctx->Bar0Length - offset)
        return STATUS_INVALID_BUFFER_SIZE;
    position = readl((PUCHAR)ctx->Bar0VirtualAddress + offset);
    if (position >= Stream->BufferSizeBytes) return STATUS_DEVICE_NOT_READY;
    *PositionBytes = position;
#else
    if (!Stream->BufferSizeBytes) return STATUS_INVALID_BUFFER_SIZE;
    *PositionBytes = Stream->LinearPositionBytes % Stream->BufferSizeBytes;
#endif
    return STATUS_SUCCESS;
}

/**
 * @brief Reads the hardware 24 MHz HD-A Wall Clock counter register (WALCLK).
 *
 * Converts 24 MHz ticks to elapsed nanoseconds:
 *   Nanoseconds = (Ticks * 1,000,000,000) / 24,000,000.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[out] ClockNanoseconds Receives wall clock time in nanoseconds.
 * @return STATUS_SUCCESS on success.
 */
NTSTATUS SofWaveRtGetClockTime(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PULONGLONG ClockNanoseconds
)
{
    if (Stream == NULL || ClockNanoseconds == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#ifdef _KERNEL_MODE
    PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
    if (!ctx || !ctx->Bar0VirtualAddress || !Stream->DescriptorPublished)
        return STATUS_DEVICE_NOT_READY;
    if (ctx->Bar0Length < SOF_HDA_REG_WALCLK + sizeof(ULONG))
        return STATUS_INVALID_BUFFER_SIZE;
    ULONG ticks = readl((PUCHAR)ctx->Bar0VirtualAddress + SOF_HDA_REG_WALCLK);
    /* All ones is indistinguishable from an inaccessible PCI register. */
    if (ticks == MAXULONG) return STATUS_DEVICE_NOT_READY;
    *ClockNanoseconds = ((ULONGLONG)ticks * 1000000000ULL) / 24000000ULL;
#else
    *ClockNanoseconds = 0;
#endif
    return STATUS_SUCCESS;
}

/**
 * @brief Registers a client notification event to be signaled on period boundaries.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT object.
 * @return STATUS_SUCCESS on success; STATUS_INSUFFICIENT_RESOURCES if table full.
 */
NTSTATUS SofWaveRtRegisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
)
{
    if (Stream == NULL || NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_SUCCESS;
#ifdef _KERNEL_MODE
    KIRQL irql;
    KeAcquireSpinLock(&Stream->NotificationLock, &irql);
#endif
    ULONG i;
    for (i = 0; i < Stream->NumNotificationEvents; ++i)
        if (Stream->NotificationEvents[i] == NotificationEvent) break;
    if (i == Stream->NumNotificationEvents) {
        if (i == SOF_WAVERT_MAX_EVENTS) status = STATUS_INSUFFICIENT_RESOURCES;
        else Stream->NotificationEvents[Stream->NumNotificationEvents++] = NotificationEvent;
    }
#ifdef _KERNEL_MODE
    KeReleaseSpinLock(&Stream->NotificationLock, irql);
#endif
    return status;
}

/**
 * @brief Unregisters a client notification event.
 *
 * @param[in] Stream            Pointer to stream instance.
 * @param[in] NotificationEvent Pointer to KEVENT object to remove.
 * @return STATUS_SUCCESS on success; STATUS_NOT_FOUND if event was not registered.
 */
NTSTATUS SofWaveRtUnregisterNotificationEvent(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ PVOID NotificationEvent
)
{
    ULONG i, j;

    if (Stream == NULL || NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_NOT_FOUND;
#ifdef _KERNEL_MODE
    KIRQL irql;
    KeAcquireSpinLock(&Stream->NotificationLock, &irql);
#endif
    for (i = 0; i < Stream->NumNotificationEvents; i++) {
        if (Stream->NotificationEvents[i] == NotificationEvent) {
            for (j = i; j < Stream->NumNotificationEvents - 1; j++) {
                Stream->NotificationEvents[j] = Stream->NotificationEvents[j + 1];
            }
            Stream->NumNotificationEvents--;
            Stream->NotificationEvents[Stream->NumNotificationEvents] = NULL;
            status = STATUS_SUCCESS;
            break;
        }
    }

#ifdef _KERNEL_MODE
    KeReleaseSpinLock(&Stream->NotificationLock, irql);
#endif
    return status;
}

/**
 * @brief Signals all registered period notification events.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtSignalNotification(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
    ULONG i;

    if (Stream == NULL) {
        return;
    }

#ifdef _KERNEL_MODE
    KIRQL irql;
    KeAcquireSpinLock(&Stream->NotificationLock, &irql);
    for (i = 0; i < Stream->NumNotificationEvents; i++) {
        if (Stream->NotificationEvents[i] != NULL) {
            KeSetEvent((PKEVENT)Stream->NotificationEvents[i], 0, FALSE);
        }
    }
    KeReleaseSpinLock(&Stream->NotificationLock, irql);
#else
    UNREFERENCED_PARAMETER(i);
#endif
}

/**
 * @brief Configures BAR0 HD-A stream registers for host audio DMA transfer.
 *
 * Sequence:
 *  1. Resets the stream engine (asserts then deasserts SRST in SD_CTL).
 *  2. Decouples host DMA directly into DSP FIFO via Processing Pipe Control (PPCTL).
 *  3. Programs 64-bit BDL Physical Address (SD_BDLPL and SD_BDLPU).
 *  4. Programs Cyclic Buffer Length (SD_CBL) and Last Valid Index (SD_LVI = 1).
 *  5. Programs computed 16-bit format word (SD_FORMAT).
 *  6. Programs Stream Tag and enables interrupt mask in SD_CTL.
 *  7. Enables stream interrupt and global interrupt in SOF_HDA_INTCTL.
 *
 * @param[in] Stream Pointer to stream instance.
 */
#ifdef _KERNEL_MODE
static NTSTATUS SofWaveRtSetHdaControl(PUCHAR Regs, ULONG Mask, ULONG Value)
{
    ULONG ctl;
    if (!Regs) return STATUS_DEVICE_NOT_READY;
    ctl = readl(Regs + SOF_HDA_ADSP_REG_SD_CTL);
    if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
    /* SD_CTL shares the top byte with W1C SD_STS. */
    writel(((ctl & 0x00ffffffu) & ~Mask) | Value, Regs + SOF_HDA_ADSP_REG_SD_CTL);
    for (ULONG i = 0; i < 500u; ++i) {
        ctl = readl(Regs + SOF_HDA_ADSP_REG_SD_CTL);
        if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
        if ((ctl & Mask) == Value) return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}
#endif

NTSTATUS SofWaveRtConfigureHdaStream(_In_ PSOF_WAVERT_STREAM Stream)
{
#ifdef _KERNEL_MODE
    PUCHAR regs = SofWaveRtHdaDescriptor(Stream);
    PSOF_DEVICE_CONTEXT ctx;
    PUCHAR ppctl;
    ULONG ctl, streamBit;
    USHORT format;
    NTSTATUS status;

    if (!regs || !Stream->DescriptorReserved || Stream->DescriptorPublished || !Stream->BdlPeriodCount)
        return STATUS_DEVICE_NOT_READY;
    ctx = Stream->DeviceContext;
    if (!ctx->PpCapOffset || ctx->PpCapOffset > ctx->Bar0Length ||
        SOF_HDA_REG_PP_PPCTL + sizeof(ULONG) > ctx->Bar0Length - ctx->PpCapOffset)
        return STATUS_INVALID_BUFFER_SIZE;
    ppctl = (PUCHAR)ctx->Bar0VirtualAddress + ctx->PpCapOffset + SOF_HDA_REG_PP_PPCTL;
    ctl = readl(ppctl);
    if (ctl == MAXULONG) return STATUS_DEVICE_NOT_READY;
    if (!Stream->DescriptorPublished) {
        ULONG descriptor = readl(regs + SOF_HDA_ADSP_REG_SD_CTL);
        if (descriptor == MAXULONG) return STATUS_DEVICE_NOT_READY;
        if ((descriptor & (SOF_HDA_SD_CTL_DMA_START | SOF_HDA_SD_CTL_DMA_RESET)) ||
            readl(regs + SOF_HDA_ADSP_REG_SD_BDLPL) ||
            readl(regs + SOF_HDA_ADSP_REG_SD_BDLPU))
            return STATUS_DEVICE_BUSY;
    }
    format = SofHdaComputeFormatWord(Stream->Format.SamplingFrequency,
        Stream->Format.ValidBitDepth, Stream->Format.ChannelsCount);
    if (!format) return STATUS_NOT_SUPPORTED;
    /* An idle descriptor has passed the ownership checks above. */
    status = SofWaveRtSetHdaControl(regs, SOF_HDA_SD_CTL_DMA_RESET, SOF_HDA_SD_CTL_DMA_RESET);
    if (!NT_SUCCESS(status)) return status;
    status = SofWaveRtSetHdaControl(regs, SOF_HDA_SD_CTL_DMA_RESET | SOF_HDA_SD_CTL_DMA_START, 0);
    if (!NT_SUCCESS(status)) return status;

    streamBit = 1u << Stream->StreamIndex;
    writel(ctl | streamBit | SOF_HDA_PPCTL_GPROCEN, ppctl);
    ctl = readl(ppctl);
    if (ctl == MAXULONG || !(ctl & streamBit)) return STATUS_DEVICE_NOT_READY;
    WRITE_REGISTER_UCHAR(regs + SOF_HDA_ADSP_REG_SD_STS,
        SOF_HDA_PERIOD_COMPLETE | SOF_HDA_SD_STS_DMA_ERRORS);
    writel(Stream->BufferSizeBytes, regs + SOF_HDA_ADSP_REG_SD_CBL);
    writew((USHORT)(Stream->BdlPeriodCount - 1u), regs + SOF_HDA_ADSP_REG_SD_LVI);
    writew(format, regs + SOF_HDA_ADSP_REG_SD_FORMAT);
    KeMemoryBarrier();
    /* Either address write can expose the BDL, even if readback fails. */
    Stream->DescriptorPublished = TRUE;
    writel(Stream->BdlPhysicalAddress.LowPart, regs + SOF_HDA_ADSP_REG_SD_BDLPL);
    writel((ULONG)Stream->BdlPhysicalAddress.HighPart, regs + SOF_HDA_ADSP_REG_SD_BDLPU);
    /* Keep controller stream IRQ disabled while the DPC polls real BCIS. */
    if (readl(regs + SOF_HDA_ADSP_REG_SD_BDLPL) != Stream->BdlPhysicalAddress.LowPart ||
        readl(regs + SOF_HDA_ADSP_REG_SD_BDLPU) != (ULONG)Stream->BdlPhysicalAddress.HighPart ||
        readl(regs + SOF_HDA_ADSP_REG_SD_CBL) != Stream->BufferSizeBytes ||
        readw(regs + SOF_HDA_ADSP_REG_SD_LVI) != Stream->BdlPeriodCount - 1u ||
        readw(regs + SOF_HDA_ADSP_REG_SD_FORMAT) != format)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return SofWaveRtSetHdaControl(regs, 0x00ffffffu, Stream->StreamTag << 20);
#else
    UNREFERENCED_PARAMETER(Stream);
    return STATUS_SUCCESS;
#endif
}

NTSTATUS SofWaveRtStartHdaStream(_In_ PSOF_WAVERT_STREAM Stream)
{
#ifdef _KERNEL_MODE
    NTSTATUS status = SofWaveRtCheckTransition(Stream, KSSTATE_RUN);
    if (!NT_SUCCESS(status)) return status;
    if (!Stream->DescriptorPublished) {
        status = SofWaveRtConfigureHdaStream(Stream);
        if (!NT_SUCCESS(status)) return status;
    }
    if (InterlockedCompareExchange(&Stream->DmaFaulted, 0, 0))
        return STATUS_IO_DEVICE_ERROR;
    return SofWaveRtSetHdaControl(SofWaveRtHdaDescriptor(Stream),
        SOF_HDA_SD_CTL_DMA_START, SOF_HDA_SD_CTL_DMA_START);
#else
    UNREFERENCED_PARAMETER(Stream);
    return STATUS_SUCCESS;
#endif
}

NTSTATUS SofWaveRtStopHdaStream(_In_ PSOF_WAVERT_STREAM Stream)
{
#ifdef _KERNEL_MODE
    if (!Stream->DescriptorPublished) return STATUS_SUCCESS;
    return SofWaveRtSetHdaControl(SofWaveRtHdaDescriptor(Stream), SOF_HDA_SD_CTL_DMA_START, 0);
#else
    UNREFERENCED_PARAMETER(Stream);
    return STATUS_SUCCESS;
#endif
}

NTSTATUS SofWaveRtResetHdaStream(_In_ PSOF_WAVERT_STREAM Stream)
{
#ifdef _KERNEL_MODE
    PUCHAR regs = SofWaveRtHdaDescriptor(Stream);
    if (!Stream->DescriptorPublished) return STATUS_SUCCESS;
    NTSTATUS status = SofWaveRtStopHdaStream(Stream);
    if (!NT_SUCCESS(status)) return status;
    status = SofWaveRtSetHdaControl(regs, SOF_HDA_SD_CTL_DMA_RESET, SOF_HDA_SD_CTL_DMA_RESET);
    if (!NT_SUCCESS(status)) return status;
    status = SofWaveRtSetHdaControl(regs, SOF_HDA_SD_CTL_DMA_RESET, 0);
    if (NT_SUCCESS(status)) {
        writel(0, regs + SOF_HDA_ADSP_REG_SD_BDLPL);
        writel(0, regs + SOF_HDA_ADSP_REG_SD_BDLPU);
        if (readl(regs + SOF_HDA_ADSP_REG_SD_BDLPL) ||
            readl(regs + SOF_HDA_ADSP_REG_SD_BDLPU) ||
            (readl(regs + SOF_HDA_ADSP_REG_SD_CTL) &
                (SOF_HDA_SD_CTL_DMA_START | SOF_HDA_SD_CTL_DMA_RESET)) ||
            SofDeviceRemoved(Stream->DeviceContext))
            return STATUS_DEVICE_NOT_READY;
        Stream->DescriptorPublished = FALSE;
    }
    return status;
#else
    UNREFERENCED_PARAMETER(Stream);
    return STATUS_SUCCESS;
#endif
}

/* =========================================================================
 * Link Stream Engine (PPLC) Operations
 * ========================================================================= */

/**
 * @brief Computes MMIO base address for a stream's Processing Pipe Link Control (PPLC) registers.
 *
 * Formula:
 *   pplcOffset = PpCapOffset + SOF_HDA_PPLC_BASE +
 *                (totalStreams * SOF_HDA_PPLC_MULTI) +
 *                (streamIndex * SOF_HDA_PPLC_INTERVAL);
 *
 * @param[in] Stream Pointer to stream instance.
 * @return MMIO address of PPLC registers, or NULL if not supported.
 */
static PUCHAR SofWaveRtGetPplcAddress(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    if (Stream != NULL && Stream->DeviceContext != NULL && Stream->DeviceContext->Bar0VirtualAddress != NULL) {
        if (Stream->EndpointType == SOF_ENDPOINT_DMIC_PDM || Stream->EndpointType == SOF_ENDPOINT_I2S_SSP2 ||
            Stream->EndpointType == SOF_ENDPOINT_PUJJO_HEADPHONE ||
            Stream->EndpointType == SOF_ENDPOINT_PUJJO_HEADSET_MIC ||
            Stream->EndpointType == SOF_ENDPOINT_PUJJO_SPEAKER) {
            return NULL;
        }
        PSOF_DEVICE_CONTEXT ctx = Stream->DeviceContext;
        if (ctx->PpCapOffset != 0) {
            PUCHAR bar0 = (PUCHAR)ctx->Bar0VirtualAddress;
            ULONG totalStreams = ctx->NumCaptureStreams + ctx->NumPlaybackStreams;
            ULONG pplcOffset = ctx->PpCapOffset + SOF_HDA_PPLC_BASE +
                               (totalStreams * SOF_HDA_PPLC_MULTI) +
                               (Stream->StreamIndex * SOF_HDA_PPLC_INTERVAL);
            return bar0 + pplcOffset;
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
    return NULL;
}

/**
 * @brief Resets the Processing Pipe Link Control (PPLC) stream DMA engine.
 *
 * Stops link DMA RUN, asserts STRST, polls for assertion, then clears STRST and polls.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtResetLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl;
        ULONG timeout;

        /* Stop link DMA first */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);

        /* Assert STRST */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl | SOF_HDA_PPLCCTL_STRST, pplc + SOF_HDA_REG_PPLCCTL);
        timeout = 50;
        while (--timeout) {
            KeStallExecutionProcessor(5);
            if (readl(pplc + SOF_HDA_REG_PPLCCTL) & SOF_HDA_PPLCCTL_STRST) break;
        }

        /* Deassert STRST */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_STRST, pplc + SOF_HDA_REG_PPLCCTL);
        timeout = 50;
        while (--timeout) {
            KeStallExecutionProcessor(5);
            if (!(readl(pplc + SOF_HDA_REG_PPLCCTL) & SOF_HDA_PPLCCTL_STRST)) break;
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Configures PPLC registers and Multi-Link (ML) capability for link streaming.
 *
 * Sequence:
 *  1. Resets the link stream.
 *  2. Programs StreamTag into PPLCCTL.
 *  3. Programs 16-bit audio format word into PPLCFMT.
 *  4. For HDMI endpoints, powers up the link (MLCTL_SPA) and asserts the stream ID
 *     in the Link Output Stream ID Valid (LOSIDV) register.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtConfigureLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl;

        /* Reset the link stream */
        SofWaveRtResetLinkStream(Stream);

        /* Program StreamTag into PPLCCTL */
        ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        ctl = (ctl & ~SOF_HDA_PPLCCTL_STRM_MASK) |
              ((Stream->StreamTag & 0x0F) << SOF_HDA_PPLCCTL_STRM_SHIFT);
        writel(ctl, pplc + SOF_HDA_REG_PPLCCTL);

        /* Program Format based on stream audio parameters */
        {
            USHORT pplcFmt = SofHdaComputeFormatWord(
                Stream->Format.SamplingFrequency,
                Stream->Format.ValidBitDepth,
                Stream->Format.ChannelsCount
            );
            writew(pplcFmt, pplc + SOF_HDA_REG_PPLCFMT);
        }

        /* If ML capability is present, configure link (only for HDMI and HDA endpoints) */
        if (Stream->DeviceContext != NULL && Stream->DeviceContext->MlCapOffset != 0 &&
            Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
            ULONG linkIndex = 0;
            if (Stream->EndpointType == SOF_ENDPOINT_HDMI_1) linkIndex = 1;
            else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_2) linkIndex = 2;
            else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_3) linkIndex = 3;

            PUCHAR bar0 = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress;
            PUCHAR ml_addr = bar0 + Stream->DeviceContext->MlCapOffset + SOF_HDA_ML_BASE +
                             (linkIndex * SOF_HDA_ML_INTERVAL);

            /* Power up link (SPA bit 16) */
            writel(readl(ml_addr + SOF_HDA_REG_ML_LCTL) | SOF_HDA_MLCTL_SPA, ml_addr + SOF_HDA_REG_ML_LCTL);

            /* If output (playback), enable stream ID in LOSIDV */
            if (!Stream->IsCapture) {
                writew(readw(ml_addr + SOF_HDA_REG_ML_LOSIDV) | (1 << Stream->StreamTag),
                       ml_addr + SOF_HDA_REG_ML_LOSIDV);
            }
        }

        {
            char bmsg[160];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] LinkStream configured: idx=%u, pplc=%p, ctl=0x%08x, fmt=0x%04x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL), readw(pplc + SOF_HDA_REG_PPLCFMT));
            SofLogBreadcrumb(bmsg);
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Starts link DMA transfer by setting SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStartLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl | SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);
        {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] StartLinkStream: idx=%u, pplc=%p, ctl=0x%08x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL));
            SofLogBreadcrumb(bmsg);
        }
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/**
 * @brief Stops link DMA transfer by clearing SOF_HDA_PPLCCTL_RUN.
 *
 * @param[in] Stream Pointer to stream instance.
 */
VOID SofWaveRtStopLinkStream(
    _In_ PSOF_WAVERT_STREAM Stream
)
{
#ifdef _KERNEL_MODE
    PUCHAR pplc = SofWaveRtGetPplcAddress(Stream);
    if (pplc != NULL) {
        ULONG ctl = readl(pplc + SOF_HDA_REG_PPLCCTL);
        writel(ctl & ~SOF_HDA_PPLCCTL_RUN, pplc + SOF_HDA_REG_PPLCCTL);
        {
            char bmsg[128];
            RtlStringCbPrintfA(bmsg, sizeof(bmsg),
                "[WaveRt] StopLinkStream: idx=%u, pplc=%p, ctl=0x%08x\n",
                Stream->StreamIndex, pplc, readl(pplc + SOF_HDA_REG_PPLCCTL));
            SofLogBreadcrumb(bmsg);
        }
    }
    if (Stream != NULL && !Stream->IsCapture && Stream->DeviceContext != NULL && Stream->DeviceContext->MlCapOffset != 0 &&
        Stream->EndpointType <= SOF_ENDPOINT_HDMI_3) {
        ULONG linkIndex = 0;
        if (Stream->EndpointType == SOF_ENDPOINT_HDMI_1) linkIndex = 1;
        else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_2) linkIndex = 2;
        else if (Stream->EndpointType == SOF_ENDPOINT_HDMI_3) linkIndex = 3;

        PUCHAR bar0 = (PUCHAR)Stream->DeviceContext->Bar0VirtualAddress;
        PUCHAR ml_addr = bar0 + Stream->DeviceContext->MlCapOffset + SOF_HDA_ML_BASE +
                         (linkIndex * SOF_HDA_ML_INTERVAL);
        writew(readw(ml_addr + SOF_HDA_REG_ML_LOSIDV) & ~(1 << Stream->StreamTag),
               ml_addr + SOF_HDA_REG_ML_LOSIDV);
    }
#else
    UNREFERENCED_PARAMETER(Stream);
#endif
}

/* =========================================================================
 * Audio Control Operations
 * ========================================================================= */

/**
 * @brief Sets stream volume level in 1/65536 dB units and sends IPC4 volume command.
 *
 * Clamps volume to [-96 dB, 0 dB]. If stream is currently active and unmuted,
 * transmits an IPC4 LARGE_CONFIG_SET message to the instantiated Gain module in DSP.
 *
 * @param[in] Stream   Pointer to stream instance.
 * @param[in] Channel  Channel index (0: left, 1: right, -1: master).
 * @param[in] VolumeDb Volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _In_ LONG VolumeDb
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG chMask;

    if (Stream == NULL || (Stream->Format.ChannelsCount > 2u && Channel != -1) ||
        (Channel != -1 && Channel != 0 && Channel != 1) ||
        (Channel == 1 && Stream->Format.ChannelsCount < 2u)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Clamp volume to [-96 dB, 0 dB] */
    if (VolumeDb > SOF_VOLUME_MAX_DB_UNITS) {
        VolumeDb = SOF_VOLUME_MAX_DB_UNITS;
    }
    if (VolumeDb < SOF_VOLUME_MIN_DB_UNITS) {
        VolumeDb = SOF_VOLUME_MIN_DB_UNITS;
    }

    chMask = Channel == -1 ? SOF_IPC4_GAIN_ALL_CHANNELS_MASK : (ULONG)Channel;
    /* If unmuted, firmware must acknowledge before accepting the new value. */
    if (Stream->IsActive && !Stream->MasterMute) {
        if (!Stream->DeviceContext || Stream->GainInstance == 0xFFFFFFFFu)
            return STATUS_DEVICE_NOT_READY;
        status = SofIpc4SetVolumeLevel(Stream->DeviceContext, Stream->GainInstance, chMask, VolumeDb);
        if (!NT_SUCCESS(status)) return status;
    }
    if (Channel == 0 || Channel == -1) Stream->VolumeLevelDb[0] = VolumeDb;
    if (Channel == 1 || Channel == -1) Stream->VolumeLevelDb[1] = VolumeDb;

    /* Update cached default in DeviceContext */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        if (Channel == 0 || Channel == -1) {
            Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][0] = Stream->VolumeLevelDb[0];
        }
        if (Channel == 1 || Channel == -1) {
            Stream->DeviceContext->DefaultVolumeLevelDb[Stream->EndpointType][1] = Stream->VolumeLevelDb[1];
        }
    }

    return status;
}

/**
 * @brief Retrieves cached volume level for the specified channel.
 *
 * @param[in]  Stream      Pointer to stream instance.
 * @param[in]  Channel     Channel index (0 or 1).
 * @param[out] VolumeDbOut Receives volume level in 1/65536 dB units.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetVolume(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG VolumeDbOut
)
{
    if (Stream == NULL || VolumeDbOut == NULL ||
        (Stream->Format.ChannelsCount > 2u && Channel != -1) ||
        (Channel != -1 && Channel != 0 && Channel != 1) ||
        (Channel == 1 && Stream->Format.ChannelsCount < 2u)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Channel == 1) {
        *VolumeDbOut = Stream->VolumeLevelDb[1];
    } else {
        *VolumeDbOut = Stream->VolumeLevelDb[0];
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Sets master mute status and dispatches IPC4 mute command to DSP Gain module.
 *
 * @param[in] Stream Pointer to stream instance.
 * @param[in] Mute   TRUE to mute; FALSE to unmute.
 * @return STATUS_SUCCESS on success, or an NTSTATUS error code.
 */
NTSTATUS SofWaveRtSetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ BOOLEAN Mute
)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Stream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Stream->IsActive) {
        status = SofWaveRtApplyCachedGain(Stream, Mute);
        if (!NT_SUCCESS(status)) return status;
    }
    Stream->MasterMute = Mute;
    Stream->IsMuted[0] = Mute;
    Stream->IsMuted[1] = Mute;

    /* Update cached default in DeviceContext */
    if (Stream->DeviceContext != NULL && Stream->EndpointType < SOF_ENDPOINT_COUNT) {
        Stream->DeviceContext->DefaultMute[Stream->EndpointType] = Mute;
    }

    return status;
}

/**
 * @brief Retrieves current master mute state.
 *
 * @param[in]  Stream  Pointer to stream instance.
 * @param[out] MuteOut Receives TRUE if muted, FALSE otherwise.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetMute(
    _In_ PSOF_WAVERT_STREAM Stream,
    _Out_ PBOOLEAN MuteOut
)
{
    if (Stream == NULL || MuteOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *MuteOut = Stream->MasterMute;
    return STATUS_SUCCESS;
}

/**
 * @brief Retrieves peak meter level for Windows Audio mixer display.
 *
 * If muted, stopped, or paused, returns -96 dB (silence). Otherwise, returns
 * current channel volume level as peak representation.
 *
 * @param[in]  Stream           Pointer to stream instance.
 * @param[in]  Channel          Channel index (0 or 1).
 * @param[out] PeakMeterDbOut   Receives peak level in 1/65536 dB units.
 * @return STATUS_SUCCESS.
 */
NTSTATUS SofWaveRtGetPeakMeter(
    _In_ PSOF_WAVERT_STREAM Stream,
    _In_ LONG Channel,
    _Out_ PLONG PeakMeterDbOut
)
{
    if (Stream == NULL || PeakMeterDbOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* If stopped, paused, or muted: return silence (-96 dB) */
    if (!Stream->IsActive || Stream->State != KSSTATE_RUN || Stream->MasterMute) {
        *PeakMeterDbOut = SOF_VOLUME_MIN_DB_UNITS;
        return STATUS_SUCCESS;
    }

    /* Active peak reporting requires a measured sample or DSP meter. */
    UNREFERENCED_PARAMETER(Channel);
    return STATUS_NOT_SUPPORTED;
}
