/* SPDX-License-Identifier: BSD-3-Clause */
#include <ntifs.h>
#if defined(PUJJO_ENABLE_EXPERIMENTAL_AUDIO) && defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG)
#include <initguid.h>
#include "PujjoAnalog.h"
#include "WaveRt.h"
#include "PujjoDmaPlan.h"
#include "PujjoIpc4Protocol.h"

typedef struct {
    PSOF_DEVICE_CONTEXT Parent;
    PUJJO_ANALOG_PROFILE Profile;
} PUJJO_ANALOG_CHILD;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PUJJO_ANALOG_CHILD, GetAnalogChild)

typedef struct _PUJJO_ANALOG_LEASE {
    PSOF_WAVERT_STREAM Stream;
    PUJJO_PERIPHERAL_INTERFACE Peripheral;
    PFILE_OBJECT PeripheralFile;
    BOOLEAN PeripheralNeedsShutdown;
    BOOLEAN JackClassified;
    BOOLEAN PositionAllowed;
    BOOLEAN Faulted;
    BOOLEAN Detached;
} PUJJO_ANALOG_LEASE;

static VOID LockAudio(PSOF_DEVICE_CONTEXT parent)
{
    KeWaitForSingleObject(&parent->AudioMutex, Executive, KernelMode, FALSE, NULL);
}

static VOID UnlockAudio(PSOF_DEVICE_CONTEXT parent)
{
    KeReleaseMutex(&parent->AudioMutex, FALSE);
}

static BOOLEAN RouteOnline(PSOF_DEVICE_CONTEXT parent)
{
    return !parent->AnalogClosed && parent->ArmedTesting && parent->EnablePortCls &&
           parent->CurrentPowerState == PowerDeviceD0 &&
           parent->FwState == SOF_FW_BOOT_COMPLETE && !parent->AudioDmaFaulted;
}

static VOID SetPositionAllowed(PSOF_DEVICE_CONTEXT parent, PUJJO_ANALOG_LEASE *lease,
                               BOOLEAN allowed)
{
    KIRQL irql;
    KeAcquireSpinLock(&parent->AudioPositionLock, &irql);
    lease->PositionAllowed = allowed;
    KeReleaseSpinLock(&parent->AudioPositionLock, irql);
}

static NTSTATUS QueryComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS OpenPeripheral(BOOLEAN speaker, PUJJO_ANALOG_LEASE *lease)
{
    PWSTR names = NULL;
    GUID classId = speaker ? GUID_DEVINTERFACE_PUJJO_AMP_CONTROL :
                             GUID_DEVINTERFACE_PUJJO_CODEC_CONTROL;
    NTSTATUS status = IoGetDeviceInterfaces(&classId, NULL, 0, &names);
    PDEVICE_OBJECT target = NULL;
    PIRP query;
    KEVENT complete;
    UNICODE_STRING path;
    PIO_STACK_LOCATION stack;

    if (!NT_SUCCESS(status)) return status;
    if (!names || !*names) {
        if (names) ExFreePool(names);
        return STATUS_NOT_FOUND;
    }
    RtlInitUnicodeString(&path, names);
    /* Board and function ID are verified by the provider. Refuse duplicate
     * matches rather than choosing a different physical codec or amplifier. */
    if (names[path.Length / sizeof(WCHAR) + 1u]) status = STATUS_OBJECT_NAME_COLLISION;
    else status = IoGetDeviceObjectPointer(&path, FILE_READ_DATA,
                                            &lease->PeripheralFile, &target);
    ExFreePool(names);
    if (!NT_SUCCESS(status)) return status;

    query = IoAllocateIrp(target->StackSize, FALSE);
    if (!query) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&complete, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(query);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_PUJJO_PERIPHERAL_CONTROL;
    stack->Parameters.QueryInterface.Version = PUJJO_PERIPHERAL_CONTROL_VERSION;
    stack->Parameters.QueryInterface.Size = (USHORT)sizeof(lease->Peripheral);
    stack->Parameters.QueryInterface.Interface = &lease->Peripheral.Header;
    query->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoSetCompletionRoutine(query, QueryComplete, &complete, TRUE, TRUE, TRUE);
    if (IoCallDriver(target, query) == STATUS_PENDING)
        KeWaitForSingleObject(&complete, Executive, KernelMode, FALSE, NULL);
    status = query->IoStatus.Status;
    IoFreeIrp(query);
    if (!NT_SUCCESS(status)) return status;

    const PUJJO_PERIPHERAL_INTERFACE *bus = &lease->Peripheral;
    if (bus->Header.Version != PUJJO_PERIPHERAL_CONTROL_VERSION ||
        bus->Header.Size != sizeof(*bus) || !bus->Header.Context ||
        !bus->Header.InterfaceReference || !bus->Header.InterfaceDereference ||
        !bus->Prepare || !bus->Shutdown || !bus->IsActive || (!speaker && !bus->ReadJack) ||
        bus->Kind != (speaker ? PUJJO_CONTROL_AMP : PUJJO_CONTROL_CODEC))
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

static VOID ClosePeripheral(PUJJO_ANALOG_LEASE *lease)
{
    if (lease->Peripheral.Header.InterfaceDereference)
        lease->Peripheral.Header.InterfaceDereference(lease->Peripheral.Header.Context);
    if (lease->PeripheralFile) ObDereferenceObject(lease->PeripheralFile);
}

static NTSTATUS StopRoute(PSOF_DEVICE_CONTEXT parent, PUJJO_ANALOG_LEASE *lease, KSSTATE state)
{
    NTSTATUS status, part;
    SetPositionAllowed(parent, lease, FALSE);
    lease->JackClassified = FALSE;
    status = SofWaveRtSetMute(lease->Stream, TRUE);
    /* Turn off the amp or restore codec registers before removing SSP clocks. */
    if (lease->PeripheralNeedsShutdown) {
        part = lease->Peripheral.Shutdown(lease->Peripheral.Header.Context, lease);
        if (NT_SUCCESS(part)) lease->PeripheralNeedsShutdown = FALSE;
        if (NT_SUCCESS(status)) status = part;
    }
    part = SofWaveRtSetState(lease->Stream, state);
    if (NT_SUCCESS(status)) status = part;
    if (!NT_SUCCESS(status)) lease->Faulted = TRUE;
    return status;
}

/* Both publication and each new stream must match the reviewed record.
 * A matching format alone cannot qualify clock divisors or reserved bits. */
static NTSTATUS VerifySspConfiguration(PSOF_DEVICE_CONTEXT parent,
                                       const PUJJO_ANALOG_PROFILE *profile)
{
    SOF_NHLT_CONFIG_KEY key = {0};
    UCHAR actual[PUJJO_REVIEWED_SSP_BYTES];
    ULONG blobBytes = 0;
    BOOLEAN nonzero = FALSE;
    NTSTATUS status;

    if (!parent->NhltTable || !parent->NhltTableSize)
        return STATUS_DEVICE_NOT_READY;
    for (ULONG i = 0; i < sizeof(profile->VerifiedSspConfig); ++i)
        if (profile->VerifiedSspConfig[i]) nonzero = TRUE;
    if (!nonzero) return STATUS_NOT_SUPPORTED;
    key.LinkType = NHLT_LINK_SSP;
    key.VirtualBusId = (UCHAR)profile->VerifiedSsp;
    key.Direction = profile->Role == PujjoAudioHeadsetMic ?
        NHLT_DIRECTION_CAPTURE : NHLT_DIRECTION_RENDER;
    key.DeviceType = NHLT_DEVICE_I2S;
    key.SampleRate = profile->Format.Rate;
    key.Channels = profile->Format.Channels;
    key.ContainerBits = profile->Format.ContainerBits;
    key.ValidBits = profile->Format.ValidBits;
    status = SofNhltCopyConfiguration(parent->NhltTable, parent->NhltTableSize,
                                       &key, actual, (ULONG)sizeof(actual), &blobBytes);
    if (!NT_SUCCESS(status)) return status;
    if (blobBytes != sizeof(actual) || (blobBytes & 3u) ||
        blobBytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD -
            (sizeof(struct sof_ipc4_copier_data) - sizeof(ULONG)))
        return STATUS_INVALID_BUFFER_SIZE;
    return RtlEqualMemory(actual, profile->VerifiedSspConfig, sizeof(actual)) ?
        STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

/* Firmware may acknowledge RUN without implementing its requested delay.
 * Keep the route muted while a separately reviewed host-side delay elapses.
 * Elapsed time is not proof that an electrical clock is present. */
static NTSTATUS WaitForHeadsetClock(const PUJJO_ANALOG_PROFILE *profile)
{
    LARGE_INTEGER interval;
    if (profile->Role == PujjoAudioSpeaker)
        return profile->HeadsetClockSettleMs == 0u ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    if (!profile->HeadsetClockSettleMs ||
        profile->HeadsetClockSettleMs > PUJJO_HEADSET_SETTLE_MAX_MS)
        return STATUS_NOT_SUPPORTED;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    interval.QuadPart = -(LONGLONG)profile->HeadsetClockSettleMs * 10000LL;
    return KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static NTSTATUS AnalogGetFormat(PVOID context, PUJJO_AUDIO_FORMAT *format)
{
    PUJJO_ANALOG_CHILD *child = context;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    LockAudio(child->Parent);
    if (RouteOnline(child->Parent)) {
        *format = child->Profile.Format;
        status = STATUS_SUCCESS;
    }
    UnlockAudio(child->Parent);
    return status;
}

static NTSTATUS AnalogAllocate(PVOID context, ULONG requested, PUJJO_AUDIO_BUFFER *buffer)
{
    const PUJJO_ANALOG_CHILD *child = context;
    PSOF_DEVICE_CONTEXT parent = child->Parent;
    const PUJJO_ANALOG_PROFILE *profile = &child->Profile;
    ULONG bytes, period;
    SOF_AUDIO_FORMAT_PARAMS format;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    PUJJO_ANALOG_LEASE *lease = NULL;
    SOF_ENDPOINT_TYPE endpoint = profile->Role == PujjoAudioSpeaker ? SOF_ENDPOINT_PUJJO_SPEAKER :
        profile->Role == PujjoAudioHeadsetMic ? SOF_ENDPOINT_PUJJO_HEADSET_MIC : SOF_ENDPOINT_PUJJO_HEADPHONE;

    RtlZeroMemory(buffer, sizeof(*buffer));
    LockAudio(parent);
    if (!RouteOnline(parent)) goto Done;
    if (parent->AnalogLease) {
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }
    status = VerifySspConfiguration(parent, profile);
    if (!NT_SUCCESS(status)) goto Done;
    SofIpc4BuildDefaultStereoFormat(profile->Format.Rate, profile->Format.ContainerBits, &format);
    format.ValidBitDepth = profile->Format.ValidBits;
    if (!pj_pcm_notification_ring(format.SamplingFrequency, format.ChannelsCount,
            format.BitDepth, format.ValidBitDepth, requested, &bytes, &period)) {
        status = STATUS_INVALID_BUFFER_SIZE;
        goto Done;
    }
    lease = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*lease), SOF_DEVICE_TAG);
    if (!lease) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    status = OpenPeripheral(profile->Role == PujjoAudioSpeaker, lease);
    if (!NT_SUCCESS(status)) goto Done;
    status = SofWaveRtCreateSspStream(parent, endpoint, profile->VerifiedSsp, &format,
                                       bytes, period, &lease->Stream);
    if (!NT_SUCCESS(status)) goto Done;

    PSOF_WAVERT_STREAM stream = lease->Stream;
    stream->Mdl = IoAllocateMdl(stream->KernelBuffer, stream->BufferSizeBytes, FALSE, FALSE, NULL);
    if (!stream->Mdl) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    MmBuildMdlForNonPagedPool(stream->Mdl);
    WdfObjectReference(parent->DmaEnabler);
    WdfObjectReference(stream->RingAllocation.CommonBuffer);
    WdfObjectReference(stream->BdlAllocation.CommonBuffer);
    stream->BuffersLeased = TRUE;
    parent->AnalogLease = lease;
    buffer->Stream = lease;
    buffer->Mdl = stream->Mdl;
    buffer->Bytes = stream->BufferSizeBytes;
    buffer->CacheType = MmCached;
Done:
    if (!NT_SUCCESS(status) && lease) {
        if (!NT_SUCCESS(SofWaveRtFreeStream(lease->Stream))) parent->AudioDmaFaulted = TRUE;
        ClosePeripheral(lease);
        ExFreePoolWithTag(lease, SOF_DEVICE_TAG);
    }
    UnlockAudio(parent);
    return status;
}

static VOID AnalogFree(PVOID context, PVOID handle)
{
    PUJJO_ANALOG_CHILD *child = context;
    PSOF_DEVICE_CONTEXT parent = child->Parent;
    PUJJO_ANALOG_LEASE *lease = handle;
    LockAudio(parent);
    if (lease && parent->AnalogLease == lease) {
        if (!lease->Detached && !NT_SUCCESS(StopRoute(parent, lease, KSSTATE_STOP)))
            parent->AudioDmaFaulted = TRUE;
        SetPositionAllowed(parent, lease, FALSE);
        if (!NT_SUCCESS(SofWaveRtFreeStream(lease->Stream))) parent->AudioDmaFaulted = TRUE;
        ClosePeripheral(lease);
        parent->AnalogLease = NULL;
        ExFreePoolWithTag(lease, SOF_DEVICE_TAG);
    }
    UnlockAudio(parent);
}

static NTSTATUS AnalogSetState(PVOID context, PVOID handle, ULONG state)
{
    const PUJJO_ANALOG_CHILD *child = context;
    PSOF_DEVICE_CONTEXT parent = child->Parent;
    const PUJJO_ANALOG_PROFILE *profile = &child->Profile;
    PUJJO_ANALOG_LEASE *lease = handle;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    PUJJO_PERIPHERAL_PROFILE settings = profile->Peripheral;

    if (state > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    LockAudio(parent);
    if (!lease || parent->AnalogLease != lease || lease->Detached || !RouteOnline(parent)) goto Done;
    if (state != KSSTATE_RUN) {
        status = StopRoute(parent, lease, (KSSTATE)state);
        goto Done;
    }
    if (lease->Faulted) goto Done;
    if (lease->PeripheralNeedsShutdown) {
        /* RUN is already qualified for the current lease. */
        if (lease->Peripheral.IsActive(lease->Peripheral.Header.Context, lease))
            status = SofWaveRtSetState(lease->Stream, KSSTATE_RUN);
        if (!NT_SUCCESS(status)) {
            lease->Faulted = TRUE;
            if (!NT_SUCCESS(StopRoute(parent, lease, KSSTATE_STOP)))
                parent->AudioDmaFaulted = TRUE;
        }
        goto Done;
    }
    SetPositionAllowed(parent, lease, FALSE);
    status = SofWaveRtSetMute(lease->Stream, TRUE);
    if (NT_SUCCESS(status) && !lease->Stream->IsCapture)
        status = SofWaveRtSetVolume(lease->Stream, -1, profile->MaximumPlaybackGain);
    if (NT_SUCCESS(status)) status = SofWaveRtSetState(lease->Stream, KSSTATE_RUN);
    if (NT_SUCCESS(status)) status = WaitForHeadsetClock(profile);
    if (NT_SUCCESS(status) && !RouteOnline(parent)) status = STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) {
        settings.DspMuted = 1u;
        settings.Codec.dsp_muted = 1u;
        settings.GainLimitApplied = 1u;
        lease->PeripheralNeedsShutdown = TRUE;
        status = lease->Peripheral.Prepare(lease->Peripheral.Header.Context, lease, &settings);
    }
    if (NT_SUCCESS(status) && profile->Role != PujjoAudioSpeaker) {
        pj5682s_jack jack = PJ5682S_JACK_ABSENT;
        status = lease->Peripheral.ReadJack(lease->Peripheral.Header.Context, lease,
            profile->Role == PujjoAudioHeadsetMic ? Pj5682sJackClassify : Pj5682sJackPresence,
            &jack);
        if (NT_SUCCESS(status) && (jack == PJ5682S_JACK_ABSENT ||
            (profile->Role == PujjoAudioHeadsetMic && jack != PJ5682S_JACK_HEADSET)))
            status = STATUS_DEVICE_NOT_CONNECTED;
        if (NT_SUCCESS(status) && jack == PJ5682S_JACK_HEADSET) lease->JackClassified = TRUE;
    }
    if (NT_SUCCESS(status) && !lease->Peripheral.IsActive(lease->Peripheral.Header.Context, lease))
        status = STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) status = SofWaveRtSetMute(lease->Stream, FALSE);
    if (NT_SUCCESS(status)) SetPositionAllowed(parent, lease, TRUE);
    else {
        lease->Faulted = TRUE;
        if (!NT_SUCCESS(StopRoute(parent, lease, KSSTATE_STOP)))
            parent->AudioDmaFaulted = TRUE;
    }
Done:
    UnlockAudio(parent);
    return status;
}

/* The idle analog blocks are unqualified. Return only a physical result
 * from the live codec and let PortCls report an unavailable query otherwise. */
static NTSTATUS AnalogJackPresent(PVOID context, PBOOLEAN present)
{
    const PUJJO_ANALOG_CHILD *child = context;
    PSOF_DEVICE_CONTEXT parent = child->Parent;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    BOOLEAN microphone = child->Profile.Role == PujjoAudioHeadsetMic;

    if (!present) return STATUS_INVALID_PARAMETER;
    LockAudio(parent);
    PUJJO_ANALOG_LEASE *lease = parent->AnalogLease;
    if (RouteOnline(parent) && lease && !lease->Detached && lease->PositionAllowed &&
        lease->Peripheral.Kind == PUJJO_CONTROL_CODEC) {
        pj5682s_jack jack;
        status = lease->Peripheral.ReadJack(lease->Peripheral.Header.Context, lease,
            microphone && lease->JackClassified ? Pj5682sJackReadClassified : Pj5682sJackPresence,
            &jack);
        if (NT_SUCCESS(status)) {
            if (jack == PJ5682S_JACK_ABSENT) lease->JackClassified = FALSE;
            if (microphone && jack == PJ5682S_JACK_PRESENT_UNCLASSIFIED)
                status = STATUS_DEVICE_NOT_READY;
            else
                *present = microphone ? jack == PJ5682S_JACK_HEADSET : jack != PJ5682S_JACK_ABSENT;
        }
    }
    UnlockAudio(parent);
    return status;
}

static NTSTATUS AnalogPosition(PVOID context, PVOID handle, PULONG position)
{
    PUJJO_ANALOG_CHILD *child = context;
    PUJJO_ANALOG_LEASE *lease = handle;
    KIRQL irql;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    KeAcquireSpinLock(&child->Parent->AudioPositionLock, &irql);
    if (lease->PositionAllowed && lease->Peripheral.IsActive(lease->Peripheral.Header.Context, lease))
        status = SofWaveRtGetPosition(lease->Stream, position);
    KeReleaseSpinLock(&child->Parent->AudioPositionLock, irql);
    return status;
}

static NTSTATUS AnalogNotification(PVOID handle, PKEVENT event, BOOLEAN add)
{
    PUJJO_ANALOG_LEASE *lease = handle;
    return add ? SofWaveRtRegisterNotificationEvent(lease->Stream, event) :
                 SofWaveRtUnregisterNotificationEvent(lease->Stream, event);
}

static VOID ChildReference(PVOID context)
{
    PUJJO_ANALOG_CHILD *child = context;
    WDFOBJECT object = WdfObjectContextGetObject(child);
    WdfObjectReference(child->Parent->WdfDevice);
    WdfObjectReference(object);
}

static VOID ChildDereference(PVOID context)
{
    PUJJO_ANALOG_CHILD *child = context;
    WDFDEVICE parent = child->Parent->WdfDevice;
    WdfObjectDereference(WdfObjectContextGetObject(child));
    WdfObjectDereference(parent);
}

NTSTATUS SofDisconnectAnalogLocked(PSOF_DEVICE_CONTEXT parent)
{
    NTSTATUS status = STATUS_SUCCESS;
    PUJJO_ANALOG_LEASE *lease = parent->AnalogLease;
    parent->AnalogClosed = TRUE;
    if (lease && !lease->Detached) {
        status = StopRoute(parent, lease, KSSTATE_STOP);
        NTSTATUS part = SofWaveRtDetachStream(lease->Stream);
        if (NT_SUCCESS(status)) status = part;
        lease->Detached = TRUE;
    }
    return status;
}

static NTSTATUS PublishAnalogChildLocked(PSOF_DEVICE_CONTEXT parent,
                                         const PUJJO_ANALOG_PROFILE *profile)
{
    static const UNICODE_STRING ids[] = {
        RTL_CONSTANT_STRING(L""),
        RTL_CONSTANT_STRING(L"PUJJO\\AudioHeadphone"),
        RTL_CONSTANT_STRING(L"PUJJO\\AudioHeadsetMic"),
        RTL_CONSTANT_STRING(L"PUJJO\\AudioSpeaker")
    };
    static const UNICODE_STRING instances[] = {
        RTL_CONSTANT_STRING(L""),
        RTL_CONSTANT_STRING(L"Headphone"),
        RTL_CONSTANT_STRING(L"HeadsetMic"),
        RTL_CONSTANT_STRING(L"Speaker")
    };
    PUJJO_AUDIO_BUS bus = {0};
    WDF_QUERY_INTERFACE_CONFIG query;
    WDF_OBJECT_ATTRIBUTES attributes;
    PWDFDEVICE_INIT init;
    WDFDEVICE child = NULL;
    NTSTATUS status;
    PUJJO_AUDIO_ROLE role;

    if (!profile || profile->Role < PujjoAudioHeadphone || profile->Role > PujjoAudioSpeaker)
        return STATUS_INVALID_PARAMETER;
    role = profile->Role;
    if (profile->Peripheral.BoardQualified != 1u || profile->Format.Role != role ||
        profile->VerifiedSsp != (role == PujjoAudioSpeaker ? 1u : 0u) ||
        profile->Format.Rate != 48000u || profile->Format.Channels != 2u ||
        profile->Format.ContainerBits != 32u || profile->Format.ValidBits != 24u ||
        profile->MaximumPlaybackGain < SOF_VOLUME_MIN_DB_UNITS ||
        profile->MaximumPlaybackGain > SOF_VOLUME_MAX_DB_UNITS)
        return STATUS_NOT_SUPPORTED;
    if (!RouteOnline(parent) || !parent->NhltTable || !parent->DmaEnabler ||
        (role == PujjoAudioHeadsetMic ? parent->NumCaptureStreams < 2u :
         parent->NumPlaybackStreams <= (role == PujjoAudioSpeaker ? 1u : 0u)) ||
        (parent->AnalogPublished & (1u << role)))
        return STATUS_DEVICE_NOT_READY;

    if ((role == PujjoAudioSpeaker && profile->HeadsetClockSettleMs != 0u) ||
        (role != PujjoAudioSpeaker && (!profile->HeadsetClockSettleMs ||
         profile->HeadsetClockSettleMs > PUJJO_HEADSET_SETTLE_MAX_MS)))
        return STATUS_NOT_SUPPORTED;

    if (role == PujjoAudioSpeaker &&
        ((profile->Peripheral.SdbPolarity != PujjoAnalogSdbActiveLow &&
          profile->Peripheral.SdbPolarity != PujjoAnalogSdbActiveHigh) ||
         profile->Peripheral.CalibrationMs < 300u || profile->Peripheral.CalibrationMs > 2000u))
        return STATUS_NOT_SUPPORTED;

    if (role != PujjoAudioSpeaker &&
        (profile->Peripheral.Codec.board_qualified != 1u ||
         profile->Peripheral.Codec.analog_path_prepared_muted != 1u ||
         profile->Peripheral.Codec.jd1_qualified != 1u ||
         !profile->Peripheral.CodecAnalog.count ||
         profile->Peripheral.CodecAnalog.count > PJ5682S_MAX_ANALOG_STEPS ||
         profile->Peripheral.Codec.lrclk_hz != profile->Format.Rate ||
         profile->Peripheral.Codec.routes !=
            (role == PujjoAudioHeadsetMic ? PJ5682S_ROUTE_ADC : PJ5682S_ROUTE_DAC)))
        return STATUS_NOT_SUPPORTED;

    status = VerifySspConfiguration(parent, profile);
    if (!NT_SUCCESS(status)) return status;

    init = WdfPdoInitAllocate(parent->WdfDevice);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    status = WdfPdoInitAssignDeviceID(init, &ids[role]);
    if (NT_SUCCESS(status)) status = WdfPdoInitAddHardwareID(init, &ids[role]);
    if (NT_SUCCESS(status)) status = WdfPdoInitAssignInstanceID(init, &instances[role]);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PUJJO_ANALOG_CHILD);
    if (NT_SUCCESS(status)) status = WdfDeviceCreate(&init, &attributes, &child);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }
    PUJJO_ANALOG_CHILD *childContext = GetAnalogChild(child);
    childContext->Parent = parent;
    childContext->Profile = *profile;
    bus.Header.Size = (USHORT)sizeof(bus);
    bus.Header.Version = PUJJO_AUDIO_INTERFACE_VERSION;
    bus.Header.Context = childContext;
    bus.Header.InterfaceReference = ChildReference;
    bus.Header.InterfaceDereference = ChildDereference;
    bus.GetFormat = AnalogGetFormat;
    bus.Allocate = AnalogAllocate;
    bus.Free = AnalogFree;
    bus.SetState = AnalogSetState;
    bus.Position = AnalogPosition;
    bus.Notification = AnalogNotification;
    if (role != PujjoAudioSpeaker) bus.JackPresent = AnalogJackPresent;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&query, &bus.Header, &GUID_PUJJO_AUDIO_BUS, NULL);
    status = WdfDeviceAddQueryInterface(child, &query);
    if (NT_SUCCESS(status)) status = WdfFdoAddStaticChild(parent->WdfDevice, child);
    if (!NT_SUCCESS(status)) WdfObjectDelete(child);
    else parent->AnalogPublished |= 1u << role;
    return status;
}
VOID SofPublishApprovedAnalogChildren(PSOF_DEVICE_CONTEXT parent)
{
    /* This table is populated only with independently reviewed Pujjo
     * evidence. No reviewed analog profile is present in this revision. */
    static const PUJJO_ANALOG_PROFILE *const approved[] = {NULL};

    LockAudio(parent);
    for (ULONG i = 0; i < RTL_NUMBER_OF(approved); ++i) {
        if (approved[i]) {
            NTSTATUS status = PublishAnalogChildLocked(parent, approved[i]);
            if (!NT_SUCCESS(status))
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                    "[PujjoAnalog] Role %lu unavailable: %08lx\n", (ULONG)approved[i]->Role, status);
        }
    }
    UnlockAudio(parent);
}
#endif
