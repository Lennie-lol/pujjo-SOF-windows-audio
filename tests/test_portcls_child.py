#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the PortCls/KMDF boundary and execute production PCM validation."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
driver = root / "src/driver"
port = (driver / "PortClsAdapter.cpp").read_text()
bus = (driver / "PujjoAudioBus.c").read_text()
wave = (driver / "WaveRt.c").read_text()
device = (driver / "Device.c").read_text()
parent = (driver / "Driver.c").read_text()

assert "PcInitializeAdapterDriver(Driver, RegistryPath, AddDevice)" in port
assert "PcAddAdapterDevice(Driver, Pdo, StartDevice, 2, 0)" in port
assert "IoCreateDriver" not in port and "DeviceExtension" not in port
assert "PcDispatchIrp" not in parent and "IOCTL_KS_" not in parent
assert "SofPortClsNotifyUsbOffload" not in (driver / "UsbOffload.c").read_text()
assert "State != KSSTATE_RUN || !ctx->AudioDmaFaulted" in bus
assert "IRP_MN_QUERY_INTERFACE" in port and "WdfDeviceAddQueryInterface(child" in bus
assert bus.index("WdfDeviceAddQueryInterface(child") < bus.index("WdfFdoAddStaticChild(")
assert port.index("RegisterPort(Device, Irp, Resources, CLSID_PortTopology") < port.index(
    "RegisterPort(Device, Irp, Resources, CLSID_PortWaveRT") < port.index("PcRegisterPhysicalConnection(")
assert port.index("PcRegisterPhysicalConnection(") < port.index("if (wavePort) UnregisterPort")
assert 'L"DmicWave"' in port and 'L"DmicTopology"' in port
assert "PUJJO\\\\AudioDmic" in (driver / "PujjoAudioBus.h").read_text()
assert r"PUJJO\AudioDmic" in (driver / "pujjo_audio.inx").read_text()
assert "AddInterface=" not in (driver / "sof_dsp.inx").read_text()
assert "g_CanaryArmed && ctx->ArmedTesting != 0 && ctx->EnablePortCls != 0" in device

disconnect = bus.split("NTSTATUS SofDisconnectAudioChild", 1)[1].split("#ifdef", 1)[0]
assert disconnect.index("AudioSetOnline(Ctx, FALSE)") < disconnect.index("SofWaveRtDetachStream")
assert "&Ctx->DmicStream" not in device  # PortCls retains the buffer lease across parent teardown.
assert bus.index("WdfObjectReference(stream->RingAllocation.CommonBuffer)") < bus.index("Buffer->Mdl =")
assert wave.index("IoFreeMdl(Stream->Mdl)") < wave.index("WdfObjectDereference(ring)")
assert "if (!stopped && !Stream->Detached)" in wave

analog = (driver / "PujjoAnalog.c").read_text()
peripheral = (driver / "peripheral/PujjoPeripheralKmdf.c").read_text()
inf = (driver / "pujjo_audio.inx").read_text()
for role in ("Headphone", "HeadsetMic", "Speaker"):
    assert "PUJJO\\Audio" + role in inf
    assert 'L"' + role + 'Topology"' in port
    assert 'L"' + role + 'Wave"' in port
assert "static const PUJJO_ANALOG_PROFILE *const approved[] = {NULL};" in analog
route = analog.split("static NTSTATUS AnalogSetState", 1)[1]
assert route.index("SofWaveRtSetMute(lease->Stream, TRUE)") < route.index(
    "lease->Peripheral.Prepare(") < route.index("SofWaveRtSetMute(lease->Stream, FALSE)")
stop = analog.split("static NTSTATUS StopRoute", 1)[1].split("static NTSTATUS AnalogGetFormat", 1)[0]
assert stop.index("SofWaveRtSetMute") < stop.index("lease->Peripheral.Shutdown") < stop.index("SofWaveRtSetState")
assert "lease->PositionAllowed && lease->Peripheral.IsActive" in analog
assert "bus.JackPresent = AnalogJackPresent" in analog
assert "KSPROPERTY_JACK_DESCRIPTION2" in port
assert "if (!NT_SUCCESS(status)) return status;" in port.split("static NTSTATUS HandleJackProperty", 1)[1]
remove = peripheral.split("static NTSTATUS RuntimeDisconnect", 1)[1]
assert remove.index("InterlockedExchangePointer(&ctx->ActiveOwner, NULL)") < remove.index("Pj5682sSpbSetPower")

# Faulted streams must not re-enter RUN or reprogram a DMA descriptor.
transition = wave[wave.index("static NTSTATUS SofWaveRtCheckTransition("):
                  wave.index("\n#endif", wave.index("static NTSTATUS SofWaveRtCheckTransition("))]
state = wave.split("NTSTATUS SofWaveRtSetState(", 1)[1]
assert state.index("SofWaveRtCheckTransition(Stream, NewState)") < state.index("switch (NewState)")
start = wave.split("NTSTATUS SofWaveRtStartHdaStream(", 1)[1]
assert start.index("SofWaveRtCheckTransition(Stream, KSSTATE_RUN)") < start.index("SofWaveRtConfigureHdaStream(Stream)")
assert "if (!NT_SUCCESS(stopStatus)) Stream->DeviceContext->AudioDmaFaulted = TRUE;" in state
transition_preamble = r'''
#include <cassert>
using NTSTATUS = int;
enum KSSTATE { KSSTATE_STOP, KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN };
constexpr int STATUS_SUCCESS=0, STATUS_DEVICE_NOT_READY=-1, STATUS_IO_DEVICE_ERROR=-2;
struct Device { bool AudioDmaFaulted; };
struct Stream { bool Detached; long DmaFaulted; Device *DeviceContext; };
using PSOF_WAVERT_STREAM = Stream *;
static long InterlockedCompareExchange(long *p, long, long) { return *p; }
'''
transition_tests = r'''
int main() {
    Device d{false};
    Stream s{false,0,&d};
    assert(SofWaveRtCheckTransition(&s,KSSTATE_RUN)==0);
    s.DmaFaulted=1;
    assert(SofWaveRtCheckTransition(&s,KSSTATE_RUN)==STATUS_IO_DEVICE_ERROR);
    assert(SofWaveRtCheckTransition(&s,KSSTATE_PAUSE)==0);
    assert(SofWaveRtCheckTransition(&s,KSSTATE_STOP)==0);
    s.DmaFaulted=0; d.AudioDmaFaulted=true;
    assert(SofWaveRtCheckTransition(&s,KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);
    assert(SofWaveRtCheckTransition(&s,KSSTATE_STOP)==0);
    d.AudioDmaFaulted=false; s.Detached=true;
    for(auto state : {KSSTATE_STOP,KSSTATE_ACQUIRE,KSSTATE_PAUSE,KSSTATE_RUN})
        assert(SofWaveRtCheckTransition(&s,state)==STATUS_DEVICE_NOT_READY);
    s.Detached=false; s.DeviceContext=nullptr;
    assert(SofWaveRtCheckTransition(&s,KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);
}
'''
transition_preamble += "#include <initializer_list>\n"

# Execute the exact leaf used by NewStream and SetFormat. The declarations
# below are host fixture types, not WDK ABI checks.
validate = port[port.index("static BOOLEAN IsPcm"):port.index("class DmicStream")]
preamble = r'''
#include <cassert>
#include <cstdint>
using ULONG=std::uint32_t; using USHORT=std::uint16_t; using NTSTATUS=int; using BOOLEAN=bool;
using GUID=unsigned;
const GUID KSDATAFORMAT_TYPE_AUDIO=1, KSDATAFORMAT_SUBTYPE_PCM=2,
           KSDATAFORMAT_SPECIFIER_WAVEFORMATEX=3;
bool IsEqualGUID(GUID a, GUID b) { return a==b; }
const int STATUS_NO_MATCH=-1, STATUS_SUCCESS=0, WAVE_FORMAT_PCM=1, WAVE_FORMAT_EXTENSIBLE=0xfffe;
const ULONG KSAUDIO_SPEAKER_STEREO=3;
struct KSDATAFORMAT { ULONG FormatSize; GUID MajorFormat,SubFormat,Specifier; };
using PKSDATAFORMAT=KSDATAFORMAT*;
#pragma pack(push,1)
struct WAVEFORMATEX {
    USHORT wFormatTag,nChannels; ULONG nSamplesPerSec,nAvgBytesPerSec;
    USHORT nBlockAlign,wBitsPerSample,cbSize;
};
struct WAVEFORMATEXTENSIBLE {
    WAVEFORMATEX Format;
    struct { USHORT wValidBitsPerSample; } Samples;
    ULONG dwChannelMask;
    GUID SubFormat;
};
struct KSDATAFORMAT_WAVEFORMATEX { KSDATAFORMAT DataFormat; WAVEFORMATEX WaveFormatEx; };
struct KSDATAFORMAT_WAVEFORMATEXTENSIBLE { KSDATAFORMAT DataFormat; WAVEFORMATEXTENSIBLE WaveFormatExt; };
#pragma pack(pop)
using PKSDATAFORMAT_WAVEFORMATEX=KSDATAFORMAT_WAVEFORMATEX*;
struct PUJJO_AUDIO_FORMAT { ULONG Rate; USHORT Channels,ContainerBits,ValidBits; };
'''
tests = r'''
int main() {
    PUJJO_AUDIO_FORMAT expected{48000,2,32,24};
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE f{};
    f.DataFormat={sizeof(f),1,2,3};
    f.WaveFormatExt={{0xfffe,2,48000,384000,8,32,22},{24},3,2};
    auto raw=&f.DataFormat;
    assert(ValidateFormat(raw,expected)==0);
    f.WaveFormatExt.Samples.wValidBitsPerSample=32;
    assert(ValidateFormat(raw,expected)==-1);
    f.WaveFormatExt.Samples.wValidBitsPerSample=24;
    f.DataFormat.FormatSize=sizeof(KSDATAFORMAT_WAVEFORMATEX);
    assert(ValidateFormat(raw,expected)==-1);
    f.DataFormat.FormatSize=sizeof(f);
    f.WaveFormatExt.Format.nAvgBytesPerSec++;
    assert(ValidateFormat(raw,expected)==-1);
    f.WaveFormatExt.Format={1,2,48000,192000,4,16,0};
    assert(ValidateFormat(raw,{48000,2,16,16})==0);
    assert(ValidateFormat(raw,{48000,2,16,12})==-1);
    assert(ValidateFormat(nullptr,expected)==-1);
    PUJJO_AUDIO_FORMAT four{48000,4,32,32};
    f.WaveFormatExt={{0xfffe,4,48000,768000,16,32,22},{32},0,2};
    assert(ValidateFormat(raw,four)==0);
    assert(PcmChannelMask(four)==0 && PcmChannelMask(expected)==3);
    f.WaveFormatExt.dwChannelMask=3;
    assert(ValidateFormat(raw,four)==-1);
    f.WaveFormatExt.dwChannelMask=0;
    f.WaveFormatExt.Format.nBlockAlign=8;
    assert(ValidateFormat(raw,four)==-1);
    f.WaveFormatExt.Format={1,4,48000,768000,16,32,0};
    assert(ValidateFormat(raw,four)==-1);
    assert(ValidateFormat(raw,{48000,4,32,24})==-1);
}
'''
jack_property = port[port.index("static NTSTATUS HandleJackProperty"):port.index("class DmicTopology")]
jack_preamble = r'''
#include <cassert>
#include <cstring>
using ULONG = unsigned;
using NTSTATUS = int;
using BOOLEAN = bool;
using PULONG = ULONG *;
constexpr BOOLEAN FALSE = false;
constexpr ULONG KSPROPERTY_TYPE_GET = 1, KSPROPERTY_TYPE_BASICSUPPORT = 2;
constexpr ULONG KSPROPERTY_JACK_DESCRIPTION2 = 3, JACKDESC2_PRESENCE_DETECT_CAPABILITY = 1;
constexpr ULONG KSAUDIO_SPEAKER_MONO = 4, KSAUDIO_SPEAKER_STEREO = 3;
constexpr ULONG eConnTypeCombination = 1, eGeoLocNotApplicable = 2, eGenLocPrimaryBox = 3, ePortConnJack = 4;
constexpr int STATUS_INVALID_DEVICE_REQUEST = -1, STATUS_BUFFER_TOO_SMALL = -2;
constexpr int STATUS_BUFFER_OVERFLOW = -3, STATUS_SUCCESS = 0;
#define PAGED_CODE() ((void)0)
#define RtlCopyMemory std::memcpy
#define NT_SUCCESS(status) ((status) >= 0)
enum PUJJO_AUDIO_ROLE { PujjoAudioHeadphone, PujjoAudioHeadsetMic };
struct Item { ULONG Id; };
struct Request {
    void *Instance; ULONG InstanceSize; ULONG Verb; void *Value; ULONG ValueSize;
    Item *PropertyItem;
};
using PPCPROPERTY_REQUEST = Request *;
struct KSMULTIPLE_ITEM { ULONG Size, Count; };
using PKSMULTIPLE_ITEM = KSMULTIPLE_ITEM *;
struct KSJACK_DESCRIPTION {
    ULONG ChannelMapping, Color, ConnectionType, GeoLocation, GenLocation, PortConnection;
    BOOLEAN IsConnected;
};
using PKSJACK_DESCRIPTION = KSJACK_DESCRIPTION *;
struct KSJACK_DESCRIPTION2 { ULONG DeviceStateInfo, JackCapabilities; };
using PKSJACK_DESCRIPTION2 = KSJACK_DESCRIPTION2 *;
struct PUJJO_AUDIO_BUS {
    struct { void *Context; } Header;
    NTSTATUS (*JackPresent)(void *, BOOLEAN *);
};
'''
jack_tests = r'''
struct Sample { NTSTATUS Status; BOOLEAN Present; unsigned Reads; };
int ReadJack(void *context, BOOLEAN *present)
{
    auto sample = static_cast<Sample *>(context);
    ++sample->Reads;
    *present = sample->Present;
    return sample->Status;
}
int main()
{
    Sample sample{-4, true, 0};
    PUJJO_AUDIO_BUS bus{{&sample}, ReadJack};
    ULONG pin = 1;
    Item item{1};
    struct { KSMULTIPLE_ITEM Many; KSJACK_DESCRIPTION Jack; } reply{};
    Request request{&pin, sizeof(pin), KSPROPERTY_TYPE_GET, &reply, sizeof(reply), &item};
    assert(HandleJackProperty(PujjoAudioHeadphone, bus, &request) == STATUS_INVALID_DEVICE_REQUEST);
    assert(!sample.Reads);
    pin = 0;
    request.ValueSize = 0;
    assert(HandleJackProperty(PujjoAudioHeadphone, bus, &request) == STATUS_BUFFER_OVERFLOW);
    assert(request.ValueSize == sizeof(reply) && !sample.Reads);
    assert(HandleJackProperty(PujjoAudioHeadphone, bus, &request) == -4);
    assert(!reply.Many.Count && !reply.Jack.IsConnected);
    sample.Status = 0;
    assert(HandleJackProperty(PujjoAudioHeadphone, bus, &request) == 0 && reply.Jack.IsConnected);
    sample.Present = false;
    assert(HandleJackProperty(PujjoAudioHeadphone, bus, &request) == 0 && !reply.Jack.IsConnected);
    struct { KSMULTIPLE_ITEM Many; KSJACK_DESCRIPTION2 Jack; } reply2{};
    item.Id = KSPROPERTY_JACK_DESCRIPTION2;
    request.Value = &reply2;
    request.ValueSize = sizeof(reply2);
    assert(HandleJackProperty(PujjoAudioHeadsetMic, bus, &request) == 0);
    assert(reply2.Many.Count == 1 && reply2.Jack.JackCapabilities == JACKDESC2_PRESENCE_DETECT_CAPABILITY);
}
'''
with tempfile.TemporaryDirectory() as temp:
    for name, body in (("pcm", preamble + validate + tests),
                        ("jack", jack_preamble + jack_property + jack_tests),
                        ("transition", transition_preamble + transition + transition_tests)):
        source = Path(temp) / (name + ".cpp")
        output = Path(temp) / name
        source.write_text(body)
        subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(source), "-o", str(output)], check=True, timeout=40
        )
        subprocess.run([str(output)], check=True, timeout=20)

package = (root / "scripts/Stage-PujjoDiagnosticPackage.ps1").read_text()
for probe in ("pujjo_rt5682_probe", "pujjo_rt1019_probe"):
    assert probe in package
    assert f"CatalogFile={probe}.cat" in (root / "packaging/diagnostics" / f"{probe}.inx").read_text()
assert "InfVerif.exe" in package and "Inf2Cat.exe" in package
assert "pnputil" not in package.lower()
print("PASS: documented PortCls child, DMA lease and teardown contracts, exact PCM, diagnostic staging")
