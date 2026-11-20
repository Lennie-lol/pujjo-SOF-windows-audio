/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2026 The SOF Authors */
#include <ntifs.h>
#include <initguid.h>
#include <portcls.h>
#include <ksmedia.h>
#include "PujjoAudioBus.h"

#define AUDIO_POOL_TAG 'aAuP'
#define DMIC_CAPTURE_PIN 1
#define DMIC_WAVE_BRIDGE 0
#define DMIC_TOPO_BRIDGE 1
#define NOTIFICATION_COUNT 2

void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag)
{
    return ExAllocatePool2(flags, size, tag);
}

void __cdecl operator delete(void* memory, size_t size)
{
    UNREFERENCED_PARAMETER(size);
    ExFreePoolWithTag(memory, AUDIO_POOL_TAG);
}

static VOID ReferenceBus(const PUJJO_AUDIO_BUS &Bus)
{
    Bus.Header.InterfaceReference(Bus.Header.Context);
}

static VOID ReleaseBus(const PUJJO_AUDIO_BUS &Bus)
{
    Bus.Header.InterfaceDereference(Bus.Header.Context);
}

static BOOLEAN IsPcm(const KSDATAFORMAT *Format)
{
    return IsEqualGUID(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
           IsEqualGUID(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
           IsEqualGUID(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX);
}

/* Raw microphone slots have no proven speaker-position mapping. */
static ULONG PcmChannelMask(const PUJJO_AUDIO_FORMAT &Format)
{
    return Format.Channels == 4u ? 0u : KSAUDIO_SPEAKER_STEREO;
}

static NTSTATUS ValidateFormat(PKSDATAFORMAT Data, const PUJJO_AUDIO_FORMAT &Expected)
{
    if ((Expected.Channels != 2u && Expected.Channels != 4u) ||
        (Expected.Channels == 4u && (Expected.ContainerBits != 32u || Expected.ValidBits != 32u)) ||
        !Data || Data->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) || !IsPcm(Data))
        return STATUS_NO_MATCH;
    const WAVEFORMATEX &format = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(Data)->WaveFormatEx;
    const USHORT block = static_cast<USHORT>(Expected.Channels * (Expected.ContainerBits / 8));
    if (format.nSamplesPerSec != Expected.Rate || format.nChannels != Expected.Channels ||
        format.wBitsPerSample != Expected.ContainerBits || format.nBlockAlign != block ||
        format.nAvgBytesPerSec != Expected.Rate * block)
        return STATUS_NO_MATCH;
    if (format.wFormatTag == WAVE_FORMAT_PCM)
        return Expected.Channels == 2u && !format.cbSize && Expected.ContainerBits == Expected.ValidBits ?
               STATUS_SUCCESS : STATUS_NO_MATCH;
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE || format.cbSize != 22 ||
        Data->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE))
        return STATUS_NO_MATCH;
    const auto extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(&format);
    return IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
           extended->Samples.wValidBitsPerSample == Expected.ValidBits &&
           extended->dwChannelMask == PcmChannelMask(Expected) ?
           STATUS_SUCCESS : STATUS_NO_MATCH;
}

class DmicStream final : public IMiniportWaveRTStreamNotification {
    LONG m_references = 1;
    PUJJO_AUDIO_BUS m_bus;
    PUJJO_AUDIO_FORMAT m_format;
    KMUTEX m_control;
    KSPIN_LOCK m_bufferLock;
    PUJJO_AUDIO_BUFFER m_buffer = {};

    void Lock() { KeWaitForSingleObject(&m_control, Executive, KernelMode, FALSE, nullptr); }
    void Unlock() { KeReleaseMutex(&m_control, FALSE); }

public:
    DmicStream(const PUJJO_AUDIO_BUS &Bus, const PUJJO_AUDIO_FORMAT &Format)
        : m_bus(Bus), m_format(Format)
    {
        ReferenceBus(m_bus);
        KeInitializeMutex(&m_control, 0);
        KeInitializeSpinLock(&m_bufferLock);
    }

    ~DmicStream()
    {
        if (m_buffer.Stream)
            FreeBufferWithNotification(m_buffer.Mdl, m_buffer.Bytes);
        ReleaseBus(m_bus);
    }

    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        *Out = nullptr;
        if (!IsEqualGUID(Id, IID_IUnknown) && !IsEqualGUID(Id, IID_IMiniportWaveRTStream) &&
            !IsEqualGUID(Id, IID_IMiniportWaveRTStreamNotification))
            return STATUS_NOINTERFACE;
        *Out = static_cast<IMiniportWaveRTStreamNotification *>(this);
        AddRef();
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_references); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG remaining = InterlockedDecrement(&m_references);
        if (!remaining) delete this;
        return remaining;
    }

    STDMETHODIMP SetFormat(PKSDATAFORMAT Format) override
    {
        return ValidateFormat(Format, m_format);
    }

    STDMETHODIMP SetState(KSSTATE State) override
    {
        Lock();
        NTSTATUS status = (State == KSSTATE_STOP || State == KSSTATE_ACQUIRE) ?
                          STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        if (m_buffer.Stream) status = m_bus.SetState(m_bus.Header.Context, m_buffer.Stream, State);
        Unlock();
        return status;
    }

    STDMETHODIMP GetPosition(PKSAUDIO_POSITION Position) override
    {
        if (!Position) return STATUS_INVALID_PARAMETER;
        KIRQL irql;
        ULONG bytes = 0;
        KeAcquireSpinLock(&m_bufferLock, &irql);
        NTSTATUS status = m_buffer.Stream ? m_bus.Position(m_bus.Header.Context,
                          m_buffer.Stream, &bytes) : STATUS_DEVICE_NOT_READY;
        KeReleaseSpinLock(&m_bufferLock, irql);
        if (NT_SUCCESS(status)) {
            Position->PlayOffset = bytes;
            Position->WriteOffset = bytes;
        }
        return status;
    }

    STDMETHODIMP AllocateAudioBuffer(ULONG Size, PMDL *Mdl, PULONG Actual,
                                     PULONG Offset, MEMORY_CACHING_TYPE *CacheType) override
    {
        return AllocateBufferWithNotification(NOTIFICATION_COUNT, Size, Mdl, Actual,
                                              Offset, CacheType);
    }

    STDMETHODIMP_(VOID) FreeAudioBuffer(PMDL Mdl, ULONG Bytes) override
    {
        FreeBufferWithNotification(Mdl, Bytes);
    }

    STDMETHODIMP_(VOID) GetHWLatency(PKSRTAUDIO_HWLATENCY Latency) override
    {
        if (Latency) RtlZeroMemory(Latency, sizeof(*Latency));
    }

    STDMETHODIMP GetPositionRegister(PKSRTAUDIO_HWREGISTER) override { return STATUS_NOT_SUPPORTED; }
    STDMETHODIMP GetClockRegister(PKSRTAUDIO_HWREGISTER) override { return STATUS_NOT_SUPPORTED; }

    STDMETHODIMP AllocateBufferWithNotification(ULONG Count, ULONG Requested, PMDL *Mdl,
        PULONG Actual, PULONG Offset, MEMORY_CACHING_TYPE *CacheType) override
    {
        if (!Mdl || !Actual || !Offset || !CacheType) return STATUS_INVALID_PARAMETER;
        *Mdl = nullptr;
        *Actual = 0;
        *Offset = 0;
        if (Count != NOTIFICATION_COUNT) return STATUS_NOT_SUPPORTED;

        Lock();
        PUJJO_AUDIO_BUFFER fresh = {};
        NTSTATUS status = m_buffer.Stream ? STATUS_DEVICE_BUSY :
            m_bus.Allocate(m_bus.Header.Context, Requested, &fresh);
        if (NT_SUCCESS(status)) {
            KIRQL irql;
            KeAcquireSpinLock(&m_bufferLock, &irql);
            m_buffer = fresh;
            KeReleaseSpinLock(&m_bufferLock, irql);
            *Mdl = fresh.Mdl;
            *Actual = fresh.Bytes;
            *CacheType = fresh.CacheType;
        }
        Unlock();
        return status;
    }

    STDMETHODIMP_(VOID) FreeBufferWithNotification(PMDL Mdl, ULONG Bytes) override
    {
        UNREFERENCED_PARAMETER(Bytes);
        Lock();
        if (m_buffer.Stream && m_buffer.Mdl == Mdl) {
            KIRQL irql;
            KeAcquireSpinLock(&m_bufferLock, &irql);
            PVOID stream = m_buffer.Stream;
            m_buffer = {};
            KeReleaseSpinLock(&m_bufferLock, irql);
            m_bus.Free(m_bus.Header.Context, stream);
        }
        Unlock();
    }

    NTSTATUS ChangeEvent(PKEVENT Event, BOOLEAN Add)
    {
        if (!Event) return STATUS_INVALID_PARAMETER;
        KIRQL irql;
        KeAcquireSpinLock(&m_bufferLock, &irql);
        NTSTATUS status = m_buffer.Stream ? m_bus.Notification(m_buffer.Stream, Event, Add) :
                                            STATUS_DEVICE_NOT_READY;
        KeReleaseSpinLock(&m_bufferLock, irql);
        return status;
    }

    STDMETHODIMP RegisterNotificationEvent(PKEVENT Event) override { return ChangeEvent(Event, TRUE); }
    STDMETHODIMP UnregisterNotificationEvent(PKEVENT Event) override { return ChangeEvent(Event, FALSE); }
};

static const KSDATARANGE_AUDIO CaptureRange = {
    {sizeof(KSDATARANGE_AUDIO), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},
    2, 16, 32, 48000, 48000
};
static const KSDATARANGE_AUDIO DmicFourRange = {
    {sizeof(KSDATARANGE_AUDIO), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},
    4, 32, 32, 48000, 48000
};
static const KSDATARANGE BridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE CaptureRanges[] = {(PKSDATARANGE)&CaptureRange};
static PKSDATARANGE DmicFourRanges[] = {(PKSDATARANGE)&DmicFourRange};
static PKSDATARANGE BridgeRanges[] = {(PKSDATARANGE)&BridgeRange};

static const PCPIN_DESCRIPTOR WavePins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, CaptureRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCPIN_DESCRIPTOR DmicFourPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, DmicFourRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCPIN_DESCRIPTOR TopologyPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE_ARRAY, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCCONNECTION_DESCRIPTOR PinConnections[] = {{PCFILTER_NODE, 0, PCFILTER_NODE, 1}};
static const GUID WaveCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO), STATICGUIDOF(KSCATEGORY_CAPTURE), STATICGUIDOF(KSCATEGORY_REALTIME)
};
static const GUID TopologyCategories[] = {STATICGUIDOF(KSCATEGORY_AUDIO)};
static const PCFILTER_DESCRIPTOR WaveFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(WavePins), WavePins,
    0, 0, nullptr, RTL_NUMBER_OF(PinConnections), PinConnections,
    RTL_NUMBER_OF(WaveCategories), WaveCategories
};
static const PCFILTER_DESCRIPTOR DmicFourFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(DmicFourPins), DmicFourPins,
    0, 0, nullptr, RTL_NUMBER_OF(PinConnections), PinConnections,
    RTL_NUMBER_OF(WaveCategories), WaveCategories
};
static const PCFILTER_DESCRIPTOR TopologyFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(TopologyPins), TopologyPins,
    0, 0, nullptr, RTL_NUMBER_OF(PinConnections), PinConnections,
    RTL_NUMBER_OF(TopologyCategories), TopologyCategories
};

static BOOLEAN IsCaptureRole(PUJJO_AUDIO_ROLE role)
{
    return role == PujjoAudioDmic || role == PujjoAudioHeadsetMic;
}

static const PCPIN_DESCRIPTOR RenderPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}},
    {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 1, CaptureRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCCONNECTION_DESCRIPTOR RenderConnections[] = {{PCFILTER_NODE, 1, PCFILTER_NODE, 0}};
static const GUID RenderCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO), STATICGUIDOF(KSCATEGORY_RENDER), STATICGUIDOF(KSCATEGORY_REALTIME)
};
static const PCFILTER_DESCRIPTOR RenderFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(RenderPins), RenderPins,
    0, 0, nullptr, RTL_NUMBER_OF(RenderConnections), RenderConnections,
    RTL_NUMBER_OF(RenderCategories), RenderCategories
};

static const PCPIN_DESCRIPTOR HeadsetTopologyPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCPIN_DESCRIPTOR HeadphoneTopologyPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_HEADPHONES, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static const PCPIN_DESCRIPTOR SpeakerTopologyPins[] = {
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_SPEAKER, nullptr, 0}},
    {0, 0, 0, nullptr, {0, nullptr, 0, nullptr, 1, BridgeRanges,
        KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, 0}}
};
static NTSTATUS PropertyJack(PPCPROPERTY_REQUEST Request);

static PCPROPERTY_ITEM JackProperties[] = {
    {&KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,
     KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyJack},
    {&KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2,
     KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyJack}
};
DEFINE_PCAUTOMATION_TABLE_PROP(JackAutomation, JackProperties);

static const PCFILTER_DESCRIPTOR HeadsetTopologyFilter = {
    0, &JackAutomation, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(HeadsetTopologyPins), HeadsetTopologyPins,
    0, 0, nullptr, RTL_NUMBER_OF(PinConnections), PinConnections,
    RTL_NUMBER_OF(TopologyCategories), TopologyCategories
};
static const PCFILTER_DESCRIPTOR HeadphoneTopologyFilter = {
    0, &JackAutomation, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(HeadphoneTopologyPins), HeadphoneTopologyPins,
    0, 0, nullptr, RTL_NUMBER_OF(RenderConnections), RenderConnections,
    RTL_NUMBER_OF(TopologyCategories), TopologyCategories
};
static const PCFILTER_DESCRIPTOR SpeakerTopologyFilter = {
    0, nullptr, sizeof(PCPIN_DESCRIPTOR), RTL_NUMBER_OF(SpeakerTopologyPins), SpeakerTopologyPins,
    0, 0, nullptr, RTL_NUMBER_OF(RenderConnections), RenderConnections,
    RTL_NUMBER_OF(TopologyCategories), TopologyCategories
};

class DmicWave final : public IMiniportWaveRT {
    LONG m_references = 1;
    PUJJO_AUDIO_BUS m_bus;
    PUJJO_AUDIO_FORMAT m_format;

public:
    DmicWave(const PUJJO_AUDIO_BUS &Bus, const PUJJO_AUDIO_FORMAT &Format)
        : m_bus(Bus), m_format(Format) { ReferenceBus(m_bus); }
    ~DmicWave() { ReleaseBus(m_bus); }

    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        *Out = nullptr;
        if (!IsEqualGUID(Id, IID_IUnknown) && !IsEqualGUID(Id, IID_IMiniport) &&
            !IsEqualGUID(Id, IID_IMiniportWaveRT))
            return STATUS_NOINTERFACE;
        *Out = static_cast<IMiniportWaveRT *>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_references); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG remaining = InterlockedDecrement(&m_references);
        if (!remaining) delete this;
        return remaining;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        *Out = (PPCFILTER_DESCRIPTOR)(m_format.Role == PujjoAudioDmic && m_format.Channels == 4u ?
            &DmicFourFilter : IsCaptureRole(m_format.Role) ? &WaveFilter : &RenderFilter);
        return STATUS_SUCCESS;
    }
    STDMETHODIMP Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT Port) override
    {
        return Port ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP GetDeviceDescription(PDEVICE_DESCRIPTION Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(Out, sizeof(*Out));
        Out->Version = DEVICE_DESCRIPTION_VERSION;
        Out->Master = TRUE;
        Out->ScatterGather = TRUE;
        Out->Dma64BitAddresses = TRUE;
        Out->InterfaceType = PCIBus;
        Out->MaximumLength = 16 * 1024 * 1024;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP DataRangeIntersection(ULONG Pin, PKSDATARANGE Client, PKSDATARANGE,
        ULONG BufferBytes, PVOID Result, PULONG Bytes) override
    {
        if (Pin != DMIC_CAPTURE_PIN) return STATUS_NOT_IMPLEMENTED;
        if (!Client || !Bytes || Client->FormatSize < sizeof(KSDATARANGE_AUDIO) || !IsPcm(Client))
            return STATUS_NO_MATCH;
        const auto range = reinterpret_cast<PKSDATARANGE_AUDIO>(Client);
        if (range->MaximumChannels < m_format.Channels ||
            range->MinimumBitsPerSample > m_format.ContainerBits ||
            range->MaximumBitsPerSample < m_format.ContainerBits ||
            range->MinimumSampleFrequency > m_format.Rate ||
            range->MaximumSampleFrequency < m_format.Rate)
            return STATUS_NO_MATCH;
        *Bytes = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        if (!BufferBytes) return STATUS_BUFFER_OVERFLOW;
        if (!Result || BufferBytes < *Bytes) return STATUS_BUFFER_TOO_SMALL;

        auto output = static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(Result);
        RtlZeroMemory(output, sizeof(*output));
        output->DataFormat.FormatSize = *Bytes;
        output->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        output->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        output->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        auto &format = output->WaveFormatExt;
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = m_format.Channels;
        format.Format.nSamplesPerSec = m_format.Rate;
        format.Format.wBitsPerSample = m_format.ContainerBits;
        format.Format.nBlockAlign = static_cast<USHORT>(m_format.Channels * (m_format.ContainerBits / 8));
        format.Format.nAvgBytesPerSec = m_format.Rate * format.Format.nBlockAlign;
        format.Format.cbSize = 22;
        format.Samples.wValidBitsPerSample = m_format.ValidBits;
        format.dwChannelMask = PcmChannelMask(m_format);
        format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        output->DataFormat.SampleSize = format.Format.nBlockAlign;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP NewStream(PMINIPORTWAVERTSTREAM *Out, PPORTWAVERTSTREAM,
                           ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT Format) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        *Out = nullptr;
        if ((!!Capture) != IsCaptureRole(m_format.Role) || Pin != DMIC_CAPTURE_PIN)
            return STATUS_NOT_SUPPORTED;
        NTSTATUS status = ValidateFormat(Format, m_format);
        if (!NT_SUCCESS(status)) return status;
        PUJJO_AUDIO_FORMAT active = {};
        status = m_bus.GetFormat(m_bus.Header.Context, &active);
        if (!NT_SUCCESS(status)) return status;
        if (active.Rate != m_format.Rate || active.Channels != m_format.Channels ||
            active.ContainerBits != m_format.ContainerBits || active.ValidBits != m_format.ValidBits ||
            active.Role != m_format.Role)
            return STATUS_DEVICE_NOT_READY;
        auto stream = new (POOL_FLAG_NON_PAGED, AUDIO_POOL_TAG) DmicStream(m_bus, m_format);
        if (!stream) return STATUS_INSUFFICIENT_RESOURCES;
        *Out = stream;
        return STATUS_SUCCESS;
    }
};

static NTSTATUS HandleJackProperty(PUJJO_AUDIO_ROLE Role, const PUJJO_AUDIO_BUS &Bus,
                                   PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();
    if (!Bus.JackPresent || !Request->Instance ||
        Request->InstanceSize < sizeof(ULONG))
        return STATUS_INVALID_DEVICE_REQUEST;
    ULONG pin;
    RtlCopyMemory(&pin, Request->Instance, sizeof(pin));
    if (pin != 0u) return STATUS_INVALID_DEVICE_REQUEST; // Physical pin on both topologies.

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Request->ValueSize < sizeof(ULONG) || !Request->Value) {
            Request->ValueSize = sizeof(ULONG);
            return STATUS_BUFFER_TOO_SMALL;
        }
        *static_cast<PULONG>(Request->Value) = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    if (!(Request->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;
    const BOOLEAN description2 = Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2;
    const ULONG bytes = sizeof(KSMULTIPLE_ITEM) +
        (description2 ? sizeof(KSJACK_DESCRIPTION2) : sizeof(KSJACK_DESCRIPTION));
    if (!Request->ValueSize) {
        Request->ValueSize = bytes;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < bytes || !Request->Value) return STATUS_BUFFER_TOO_SMALL;

    BOOLEAN present = FALSE;
    NTSTATUS status = Bus.JackPresent(Bus.Header.Context, &present);
    if (!NT_SUCCESS(status)) return status;
    auto multiple = static_cast<PKSMULTIPLE_ITEM>(Request->Value);
    multiple->Size = bytes;
    multiple->Count = 1;
    if (description2) {
        auto desc = reinterpret_cast<PKSJACK_DESCRIPTION2>(multiple + 1);
        desc->DeviceStateInfo = 0;
        desc->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;
    } else {
        auto desc = reinterpret_cast<PKSJACK_DESCRIPTION>(multiple + 1);
        desc->ChannelMapping = Role == PujjoAudioHeadsetMic ?
                                KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
        desc->Color = 0;
        desc->ConnectionType = eConnTypeCombination;
        desc->GeoLocation = eGeoLocNotApplicable;
        desc->GenLocation = eGenLocPrimaryBox;
        desc->PortConnection = ePortConnJack;
        desc->IsConnected = present;
    }
    Request->ValueSize = bytes;
    return STATUS_SUCCESS;
}

class DmicTopology final : public IMiniportTopology {
    LONG m_references = 1;
    PUJJO_AUDIO_ROLE m_role;
    PUJJO_AUDIO_BUS m_bus;

public:
    DmicTopology(PUJJO_AUDIO_ROLE Role, const PUJJO_AUDIO_BUS &Bus) : m_role(Role), m_bus(Bus)
    {
        ReferenceBus(m_bus);
    }
    ~DmicTopology() { ReleaseBus(m_bus); }

    NTSTATUS GetJack(PPCPROPERTY_REQUEST Request)
    {
        return HandleJackProperty(m_role, m_bus, Request);
    }

    STDMETHODIMP QueryInterface(REFIID Id, PVOID *Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        *Out = nullptr;
        if (!IsEqualGUID(Id, IID_IUnknown) && !IsEqualGUID(Id, IID_IMiniport) &&
            !IsEqualGUID(Id, IID_IMiniportTopology))
            return STATUS_NOINTERFACE;
        *Out = static_cast<IMiniportTopology *>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_references); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG remaining = InterlockedDecrement(&m_references);
        if (!remaining) delete this;
        return remaining;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *Out) override
    {
        if (!Out) return STATUS_INVALID_PARAMETER;
        switch (m_role) {
        case PujjoAudioHeadphone: *Out = (PPCFILTER_DESCRIPTOR)&HeadphoneTopologyFilter; break;
        case PujjoAudioHeadsetMic: *Out = (PPCFILTER_DESCRIPTOR)&HeadsetTopologyFilter; break;
        case PujjoAudioSpeaker: *Out = (PPCFILTER_DESCRIPTOR)&SpeakerTopologyFilter; break;
        default: *Out = (PPCFILTER_DESCRIPTOR)&TopologyFilter; break;
        }
        return STATUS_SUCCESS;
    }
    STDMETHODIMP Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY Port) override
    {
        return Port ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE,
                                      ULONG, PVOID, PULONG) override
    {
        return STATUS_NOT_IMPLEMENTED;
    }
};

static NTSTATUS PropertyJack(PPCPROPERTY_REQUEST Request)
{
    if (!Request || !Request->MajorTarget || !Request->PropertyItem)
        return STATUS_INVALID_PARAMETER;
    return static_cast<DmicTopology *>(Request->MajorTarget)->GetJack(Request);
}

static NTSTATUS CompleteQuery(PDEVICE_OBJECT, PIRP, PVOID Context)
{
    KeSetEvent(static_cast<PKEVENT>(Context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS QueryAudioBus(PDEVICE_OBJECT Device, PUJJO_AUDIO_BUS *Bus)
{
    PDEVICE_OBJECT lower = IoGetLowerDeviceObject(Device);
    if (!lower) return STATUS_NO_SUCH_DEVICE;
    PIRP irp = IoAllocateIrp(lower->StackSize, FALSE);
    if (!irp) {
        ObDereferenceObject(lower);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_PUJJO_AUDIO_BUS;
    stack->Parameters.QueryInterface.Size = static_cast<USHORT>(sizeof(*Bus));
    stack->Parameters.QueryInterface.Version = PUJJO_AUDIO_INTERFACE_VERSION;
    stack->Parameters.QueryInterface.Interface = &Bus->Header;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoSetCompletionRoutine(irp, CompleteQuery, &done, TRUE, TRUE, TRUE);
    if (IoCallDriver(lower, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    ObDereferenceObject(lower);
    return status;
}

static NTSTATUS RegisterPort(PDEVICE_OBJECT Device, PIRP Irp, PRESOURCELIST Resources,
    REFCLSID ClassId, PCWSTR Name, PUNKNOWN Miniport, PPORT *Out)
{
    PPORT port = nullptr;
    NTSTATUS status = PcNewPort(&port, ClassId);
    if (NT_SUCCESS(status)) status = port->Init(Device, Irp, Miniport, nullptr, Resources);
    if (NT_SUCCESS(status)) status = PcRegisterSubdevice(Device, const_cast<PWSTR>(Name), port);
    if (NT_SUCCESS(status)) *Out = port;
    else if (port) port->Release();
    return status;
}

static VOID UnregisterPort(PDEVICE_OBJECT Device, PPORT Port)
{
    IUnregisterSubdevice *unregister = nullptr;
    NTSTATUS status = Port->QueryInterface(IID_IUnregisterSubdevice,
                                           reinterpret_cast<PVOID *>(&unregister));
    if (NT_SUCCESS(status)) {
        status = unregister->UnregisterSubdevice(Device, Port);
        unregister->Release();
    }
    if (!NT_SUCCESS(status))
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[PujjoAudio] Port startup rollback failed: %08lx\n", status);
}

/* PortCls owns the FDO, its PnP dispatch and filter cleanup. The capture PDO
 * publishes the DSP bus only after its parent has entered D0. */
static NTSTATUS StartDevice(PDEVICE_OBJECT Device, PIRP Irp, PRESOURCELIST Resources)
{
    PUJJO_AUDIO_BUS bus = {};
    PUJJO_AUDIO_FORMAT format = {};
    NTSTATUS status = QueryAudioBus(Device, &bus);
    if (!NT_SUCCESS(status)) return status;
    if (bus.Header.Size != sizeof(bus) || bus.Header.Version != PUJJO_AUDIO_INTERFACE_VERSION ||
        !bus.Header.Context || !bus.Header.InterfaceReference || !bus.Header.InterfaceDereference ||
        !bus.GetFormat || !bus.Allocate || !bus.Free || !bus.SetState || !bus.Position ||
        !bus.Notification) {
        if (bus.Header.InterfaceDereference) ReleaseBus(bus);
        return STATUS_INVALID_DEVICE_STATE;
    }

    PPORT topologyPort = nullptr, wavePort = nullptr;
    DmicWave *wave = nullptr;
    DmicTopology *topology = nullptr;
    static const PCWSTR topologyNames[] = {
        L"DmicTopology", L"HeadphoneTopology", L"HeadsetMicTopology", L"SpeakerTopology"
    };
    static const PCWSTR waveNames[] = {
        L"DmicWave", L"HeadphoneWave", L"HeadsetMicWave", L"SpeakerWave"
    };
    status = bus.GetFormat(bus.Header.Context, &format);
    if (NT_SUCCESS(status) && (unsigned)format.Role >= RTL_NUMBER_OF(waveNames))
        status = STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(status)) {
        topology = new (POOL_FLAG_NON_PAGED, AUDIO_POOL_TAG) DmicTopology(format.Role, bus);
        wave = new (POOL_FLAG_NON_PAGED, AUDIO_POOL_TAG) DmicWave(bus, format);
        if (!topology || !wave) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status))
        status = RegisterPort(Device, Irp, Resources, CLSID_PortTopology, topologyNames[format.Role],
                              topology, &topologyPort);
    if (NT_SUCCESS(status))
        status = RegisterPort(Device, Irp, Resources, CLSID_PortWaveRT, waveNames[format.Role],
                              wave, &wavePort);
    if (NT_SUCCESS(status)) {
        if (IsCaptureRole(format.Role))
            status = PcRegisterPhysicalConnection(Device, topologyPort, DMIC_TOPO_BRIDGE,
                                                  wavePort, DMIC_WAVE_BRIDGE);
        else
            status = PcRegisterPhysicalConnection(Device, wavePort, DMIC_WAVE_BRIDGE,
                                                  topologyPort, DMIC_TOPO_BRIDGE);
    }

    if (!NT_SUCCESS(status)) {
        if (wavePort) UnregisterPort(Device, wavePort);
        if (topologyPort) UnregisterPort(Device, topologyPort);
    }
    if (wavePort) wavePort->Release();
    if (topologyPort) topologyPort->Release();
    if (wave) wave->Release();
    if (topology) topology->Release();
    ReleaseBus(bus);
    return status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    return PcAddAdapterDevice(Driver, Pdo, StartDevice, 2, 0);
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    return PcInitializeAdapterDriver(Driver, RegistryPath, AddDevice);
}
