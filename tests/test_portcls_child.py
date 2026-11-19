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
}
'''
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp) / "portcls_pcm.cpp"
    output = Path(temp) / "portcls_pcm"
    source.write_text(preamble + validate + tests)
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
