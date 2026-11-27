#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute the pure WDK-linked DMA/PCM planner; no real DMA or device access."""
import os, shlex, shutil, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC") or next((v for v in ("clang","gcc","cc") if shutil.which(v)), None)
if not cc:
    raise SystemExit("FAIL: C compiler missing")
main = r'''
#include "PujjoDmaPlan.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    pj_dma_plan p;
    PJ_DWORD period=99u;
    assert(pj_dma_plan_pair(0xffffff80u, 1u, 512u, 256u, &p));
    assert(pj_hda_dma_span_valid(0xffffff80u,0,128,0));
    assert(!pj_hda_dma_span_valid(0xffffff80u,0,129,0));
    assert(pj_hda_dma_span_valid(0xffffff80u,0,129,1));
    assert(!pj_hda_dma_span_valid(0,1,1,0));
    assert(!pj_hda_dma_span_valid(0xffffff80u,0xffffffffu,129,1));
    assert(!pj_hda_dma_span_valid(1,0,128,1));
    assert(!pj_hda_dma_span_valid(0,0,0,1));
    assert(p.first.low==0xffffff80u && p.first.high==1u && p.first.length==256u);
    assert(p.second.low==128u && p.second.high==2u && p.second.length==256u);
    assert(!pj_dma_plan_pair(0xffffff80u,0xffffffffu,512u,256u,&p));
    assert(p.second.high==2u);
    assert(!pj_dma_plan_pair(1u,0u,512u,256u,&p));
    assert(!pj_dma_plan_pair(0u,0u,128u,64u,&p));
    assert(!pj_dma_plan_pair(0u,0u,512u,0u,&p));
    assert(!pj_dma_plan_pair(0u,0u,512u,512u,&p));
    assert(!pj_dma_plan_pair(0u,0u,512u,129u,&p));
    assert(!pj_dma_plan_pair(0u,0u,PJ_DMA_MAX_BYTES+1u,256u,&p));
    assert(!pj_dma_plan_pair(0u,0u,512u,256u,NULL));
    PJ_DWORD count=99u;
    pj_dma_segment segment={0};
    assert(pj_dma_period_count(0xffffff80u,1u,512u,128u,&count) && count==4u);
    assert(pj_dma_period_segment(0xffffff80u,1u,512u,128u,1u,&segment));
    assert(segment.low==0u && segment.high==2u && segment.length==128u);
    assert(!pj_dma_period_segment(0xffffff80u,1u,512u,128u,4u,&segment));
    assert(!pj_dma_period_count(0xffffff80u,0xffffffffu,512u,128u,&count));
    assert(!pj_dma_period_count(0u,0u,512u,384u,&count));
    assert(!pj_dma_period_count(0u,0u,32768u+128u,128u,&count));
    assert(!pj_dma_period_count(0u,0u,128u,128u,&count));
    assert(count==4u && segment.high==2u);
    assert(pj_pcm_period(48000u,2u,32u,24u,10u,65536u,&period) && period==3840u);
    assert(!pj_pcm_period(48000u,0u,32u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,16u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(44100u,2u,32u,24u,1u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,32u,24u,10u,256u,&period));
    assert(!pj_pcm_period(48000u,2u,24u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(0xffffffffu,8u,32u,32u,1000u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,32u,24u,10u,65536u,NULL));
    assert(period==3840u);
    for (PJ_DWORD n=0;n<=15u;++n) {
        for (PJ_DWORD m=0;m<=15u;++m) {
            for (PJ_DWORD slot=0;slot<=15u;++slot) {
                PJ_DWORD idx=99u;
                assert(pj_hda_stream_slot(n,m,1u,slot,4096u,&idx)==(slot<n));
                assert(idx==(slot<n ? slot : 99u));
                idx=99u;
                assert(pj_hda_stream_slot(n,m,0u,slot,4096u,&idx)==(slot<m));
                assert(idx==(slot<m ? n+slot : 99u));
            }
        }
    }
    PJ_DWORD idx=99u;
    assert(!pj_hda_stream_slot(2,3,0,0,0xdf,&idx) && idx==99u);
    assert(pj_hda_stream_slot(2,3,0,0,0xe0,&idx) && idx==2u);
    assert(!pj_hda_stream_slot(16,2,1,0,4096,&idx));
    assert(!pj_hda_stream_slot(2,16,0,0,4096,&idx));
    assert(!pj_hda_stream_slot(2,3,2,0,4096,&idx));
    assert(!pj_hda_stream_slot(2,3,1,0,4096,NULL));
    PJ_DWORD ring=0, ms=0, period_bytes=0;
    assert(pj_pcm_notification_ring(48000u,2u,32u,24u,65536u,&ring,&ms));
    assert(ring==66048u && ms==86u);
    assert(pj_pcm_period(48000u,2u,32u,24u,ms,ring,&period_bytes));
    assert(period_bytes*2u==ring);
    assert(!pj_pcm_notification_ring(48000u,2u,24u,24u,65536u,&ring,&ms));
    assert(!pj_pcm_notification_ring(48000u,2u,32u,24u,0u,&ring,&ms));
    assert(ring==66048u && ms==86u);
    assert(pj_pcm_notification_ring(48000u,4u,32u,32u,65536u,&ring,&ms));
    assert(pj_pcm_period(48000u,4u,32u,32u,ms,ring,&period_bytes));
    assert(period_bytes==768u*ms && ring==2u*period_bytes && ring>=65536u);
    puts("PASS: DMA carry, span overflow, alignment, and checked PCM geometry");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dma_plan_") as tmp:
    test=Path(tmp)/"test.c"
    exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
    test.write_text(main)
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-Wconversion",
                    "-Wsign-conversion","-pedantic",*shlex.split(os.environ.get("CFLAGS","")),"-I"+str(root/"src/driver"),
                    str(root/"src/driver/PujjoDmaPlan.c"),str(test),"-o",str(exe)],
                   check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=10)
loader=(root/"src/driver/Loader.c").read_text().split("NTSTATUS\nSofConfigureCodeLoaderDma(",1)[1]
assert loader.index("pj_dma_plan_pair(")<loader.index("gctl = readl")
assert "bdl[1].AddrHigh = plan.second.high;" in loader
device=(root/"src/driver/Device.c").read_text().split("SofEvtDevicePrepareHardware(",1)[1]
assert device.index("!SofHardwareDevelopmentAllowed()")<device.index("WdfFdoQueryForInterface")
wave=(root/"src/driver/WaveRt.c").read_text()
assert "stream->PeriodSizeBytes = periodBytes;" in wave
assert "#ifndef PUJJO_ENABLE_EXPERIMENTAL_AUDIO" in wave
assert wave.index("#ifndef PUJJO_ENABLE_EXPERIMENTAL_AUDIO") < wave.index("pj_hda_stream_slot(") < wave.index("ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SOF_WAVERT_STREAM)")
assert "stream->StreamIndex = descriptorIndex;" in wave
assert "operating in silence capture mode" not in wave
for name in ("Driver.c","PortClsAdapter.cpp"):
    src=(root/"src/driver"/name).read_text()
    assert "InterlockedExchange(&" not in src or "->ReferenceCount, 100000" not in src
    assert "i < 100000" not in src
print("PASS: production source gates and geometry integration")

# Compile the exact arithmetic helper from Topo.c with only fixed-width host
# types. This checks arithmetic/control flow, not WDK structure layout or I/O.
topo=(root/"src/driver/Topo.c").read_text()
helper="static NTSTATUS SofIpc4ComputeChunkBytes("+topo.split(
    "static NTSTATUS SofIpc4ComputeChunkBytes(",1)[1].split("\n}",1)[0]+"\n}\n"
fixture=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef uint32_t ULONG, *PULONG;
typedef int NTSTATUS;
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_SUCCESS 0
#define SOF_IPC4_CHANNELS_INTERLEAVED 0u
typedef struct {
    ULONG SamplingFrequency, BitDepth, ValidBitDepth, ChannelsCount;
    ULONG InterleavingStyle, ChannelMap, ChannelConfig;
} SOF_AUDIO_FORMAT_PARAMS;
'''
cases=r'''
int main(void) {
    SOF_AUDIO_FORMAT_PARAMS f={48000u,32u,24u,2u,0u,0u,0u};
    ULONG n=17u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_SUCCESS && n==384u);
    f.ChannelsCount=4u; f.ValidBitDepth=32u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_SUCCESS && n==768u);
    f.ChannelsCount=2u;
    f.SamplingFrequency=44100u;
    f.BitDepth=16u;
    f.ValidBitDepth=16u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_SUCCESS && n==180u);
    f.ChannelsCount=1u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_SUCCESS && n==90u);
    f.SamplingFrequency=192000u;
    f.ChannelsCount=8u;
    f.BitDepth=32u;
    f.ValidBitDepth=32u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_SUCCESS && n==6144u);
    f.ChannelsCount=9u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_INVALID_PARAMETER && n==6144u);
    f.ChannelsCount=2u;
    f.SamplingFrequency=UINT32_MAX;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_INVALID_PARAMETER && n==6144u);
    f.SamplingFrequency=48000u;
    f.ValidBitDepth=33u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_INVALID_PARAMETER);
    f.ValidBitDepth=24u;
    f.InterleavingStyle=1u;
    assert(SofIpc4ComputeChunkBytes(&f,&n)==STATUS_INVALID_PARAMETER);
    assert(SofIpc4ComputeChunkBytes(NULL,&n)==STATUS_INVALID_PARAMETER);
    assert(SofIpc4ComputeChunkBytes(&f,NULL)==STATUS_INVALID_PARAMETER);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pj_topo_chunk_") as tmp:
    test=Path(tmp)/"test.c"
    exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
    test.write_text(fixture+helper+cases)
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-Wconversion",
                    "-Wsign-conversion","-pedantic",str(test),"-o",str(exe)],
                   check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=10)
assert topo.count("SofIpc4BuildGainConfigForFormat(Fmt, SOF_IPC4_VOLUME_MUTE,")==6
gain=topo.split("static NTSTATUS SofIpc4BuildGainConfigForFormat(",1)[1].split("\n}",1)[0]
assert "SofIpc4BuildAudioFormat(Fmt," in gain
copier=topo.split("NTSTATUS SofIpc4BuildCopierConfig(",1)[1].split("\n}",1)[0]
assert copier.index("configBytes = sizeof(struct sof_ipc4_alh_configuration_blob)") < copier.index("totalSize += configBytes") < copier.index("if (MaxBuffer < totalSize)") < copier.index("RtlZeroMemory(CopierBuffer, totalSize)")
assert "chunkBytes = 384" not in copier and "chunkBytes = 384" not in gain
print("PASS: IPC4 frame-aligned copier/gain geometry and bounded ALH blob")

# WaveRT lifecycle must not publish RUN or free notification context early.
state=wave.split("NTSTATUS SofWaveRtSetState(",1)[1].split("\n}",1)[0]
run=state.split("case KSSTATE_RUN:",1)[1].split("case KSSTATE_PAUSE:",1)[0]
assert run.index("status = SofIpc4SetPipelineState") < run.index("if (!NT_SUCCESS(status)) goto run_failed;",run.index("status = SofIpc4SetPipelineState")) < run.index("Stream->State = KSSTATE_RUN;") < run.index("KeSetTimerEx")
rollback=state.split("run_failed:",1)[1]
assert "SofWaveRtStopHdaStream(Stream)" in rollback and "SofWaveRtStopLinkStream(Stream)" in rollback
assert "Stream->IsActive = FALSE;" in rollback and "Stream->State = KSSTATE_STOP;" in rollback
free=wave.split("NTSTATUS SofWaveRtFreeStream(",1)[1].split("\n}",1)[0]
assert free.index("KeCancelTimer") < free.index("KeRemoveQueueDpc") < free.index("KeFlushQueuedDpcs") < free.index("ExFreePoolWithTag")
assert free.index("if (NT_SUCCESS(deleteStatus))") < free.index("SofIpc4FreeInstanceId")
print("PASS: WaveRT lifecycle ordering and failed deletion ownership guards")

# Ensure code loader validates controller and optional capability registers
# before any MMIO, and never publishes DMA descriptors after a reset timeout.
configure=loader.split("\n/**",1)[0]
assert configure.index("capOffset = Ctx->SpbCapOffset") < configure.index("gctl = readl")
assert "spibRelative > Ctx->Bar0Length - capOffset" in configure
assert "gctl == 0xFFFFFFFFu || !(gctl & SOF_HDA_GCTL_RESET)" in configure
assert configure.count("for (timeout = 0; timeout < 500u; ++timeout)")==2
assert configure.index("if (timeout == 500u) return STATUS_IO_TIMEOUT;") < configure.index("/* Only decouple")
assert configure.index("if (timeout == 500u || (ctl & SOF_HDA_SD_CTL_DMA_START))") < configure.index("WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL)")
assert "ctl = StreamTag << 20;" in configure
assert "bdl[1].Ioc = 0;" in configure
assert "intctl & ~(1u << streamIndex)" in configure
assert "SOF_HDA_DPIB_BASE + Stream->StreamIndex * SOF_HDA_DPIB_STRIDE" in wave
print("PASS: code loader BAR preflight, required reset acknowledgment and polled stream IRQ ownership")

# Exercise the production control helpers against an HDA DWORD whose upper
# byte implements write-one-to-clear status.
loader_source=(root/"src/driver/Loader.c").read_text()
control_helpers=loader_source.split("static ULONG SofCodeLoaderReadControl(",1)[1].split("\n/**",1)[0]
control_fixture=r'''
#include <assert.h>
#include <stdint.h>
typedef uint32_t ULONG, *PULONG;
typedef unsigned char *PUCHAR;
typedef void VOID;
#define _In_
#define SOF_HDA_ADSP_REG_SD_CTL 0
#define READ_REGISTER_ULONG(reg) (*(reg))
static void WRITE_REGISTER_ULONG(PULONG reg, ULONG value)
{
    *reg = (*reg & 0xff000000u & ~value) | (value & 0x00ffffffu);
}
'''
control_cases=r'''
int main(void)
{
    ULONG reg = 0x1c500000u;
    PUCHAR stream = (PUCHAR)&reg;
    ULONG ctl = SofCodeLoaderReadControl(stream);
    assert(ctl == 0x00500000u);
    SofCodeLoaderWriteControl(stream, ctl | 2u);
    assert(reg == 0x1c500002u);
    SofCodeLoaderWriteControl(stream, reg & ~2u);
    assert(reg == 0x1c500000u);
    SofCodeLoaderWriteControl(stream, 1u);
    assert(reg == 0x1c000001u);
    reg = UINT32_MAX;
    assert(SofCodeLoaderReadControl(stream) == UINT32_MAX);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pj_loader_control_") as tmp:
    test=Path(tmp)/"test.c"
    exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
    test.write_text(control_fixture+"static ULONG SofCodeLoaderReadControl("+control_helpers+control_cases)
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror",str(test),"-o",str(exe)],
                   check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=10)
assert loader_source.count("WRITE_REGISTER_ULONG((PULONG)(StreamRegs + SOF_HDA_ADSP_REG_SD_CTL)") == 1
assert "streamRegs + SOF_HDA_ADSP_REG_SD_CTL" not in loader_source
assert configure.index("if (intctl == 0xFFFFFFFFu || ppctl == 0xFFFFFFFFu || spbfcctl == 0xFFFFFFFFu)") < configure.index("WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL)")
assert configure.index("FwDmaBuf->Published = TRUE;") < configure.index("BdlDmaBuf->Published = TRUE;") < configure.index("*OutStreamIndex = streamIndex;") < configure.index("WRITE_REGISTER_ULONG((PULONG)(streamRegs + SOF_HDA_ADSP_REG_SD_BDLPL)")
print("PASS: code loader control preserves W1C status and rejects missing MMIO before BDL publication")

# A failed gain request must not publish a successful control or stream.
apply=wave.split("static NTSTATUS SofWaveRtApplyCachedGain(",1)[1].split("\n}",1)[0]
assert "Stream->Format.ChannelsCount != 2u" in apply
assert "1u, Stream->VolumeLevelDb[1]" in apply
assert apply.index("if (!NT_SUCCESS(status))") < apply.index("(void)SofIpc4SetMute")
create=wave.split("NTSTATUS SofWaveRtCreateStreamCore(",1)[1].split("\n}",1)[0]
assert create.index("status = SofWaveRtApplyCachedGain") < create.index("SofWaveRtConfigureHdaStream(stream)") < create.index("stream->IsActive = TRUE;")
volume=wave.split("NTSTATUS SofWaveRtSetVolume(",1)[1].split("\n}",1)[0]
assert volume.index("if (!NT_SUCCESS(status)) return status;") < volume.index("Stream->VolumeLevelDb[0] = VolumeDb;")
mute=wave.split("NTSTATUS SofWaveRtSetMute(",1)[1].split("\n}",1)[0]
assert mute.index("if (!NT_SUCCESS(status)) return status;") < mute.index("Stream->MasterMute = Mute;")
bus=(root/"src/driver/PujjoAudioBus.c").read_text()
capture_stop=bus.split("static NTSTATUS AudioStopCapture(",1)[1].split("\n}",1)[0]
assert capture_stop.index("SofWaveRtSetMute(stream, TRUE)") < capture_stop.index("transition = SofWaveRtSetState(stream, state);")
assert capture_stop.index("transition = SofWaveRtSetState(stream, state);") < capture_stop.index("if (NT_SUCCESS(status)) status = transition;")
assert "if (!NT_SUCCESS(status)) ctx->AudioDmaFaulted = TRUE;" in capture_stop
capture_state=bus.split("static NTSTATUS AudioSetState(",1)[1].split("\n}",1)[0]
assert capture_state.index("status = SofWaveRtSetMute(stream, FALSE);") < capture_state.index("(void)AudioStopCapture(ctx, stream, KSSTATE_STOP);") < capture_state.index("AudioSetPositionEnabled(ctx, TRUE);")
assert "if (!NT_SUCCESS(status))\n                (void)AudioStopCapture(ctx, stream, KSSTATE_STOP);" in capture_state
print("PASS: gain error propagation, per-channel restoration and control publication")

for name in ("RegisterNotificationEvent", "UnregisterNotificationEvent", "SignalNotification"):
    body = wave.split("SofWaveRt"+name+"(\n",1)[1].split("\n}",1)[0]
    assert body.index("KeAcquireSpinLock") < body.index("Stream->NotificationEvents[") < body.index("KeReleaseSpinLock")
position = wave.split("NTSTATUS SofWaveRtGetPosition(",1)[1].split("#else",1)[0]
assert position.index("ctx->Bar0Length - offset") < position.index("position = readl") < position.index("*PositionBytes = position")
assert "LinearPositionBytes +=" not in wave
assert "MmGetPhysicalAddress(" not in wave and "MmAllocateContiguousMemory(" not in wave
assert "stream->Bdl[i].AddrHigh = segment.high" in wave
assert "writew((USHORT)(Stream->BdlPeriodCount - 1u)" in wave
timer = wave.split("static VOID SofWaveRtTimerDpc(",1)[1].split("\n}",1)[0]
assert timer.index("flags & SOF_HDA_SD_STS_DMA_ERRORS") < timer.index("flags & SOF_HDA_PERIOD_COMPLETE")
assert timer.index("flags & SOF_HDA_PERIOD_COMPLETE") < timer.index("SofWaveRtSignalNotification(stream)")
stop = wave.split("NTSTATUS SofWaveRtStopHdaStream(",1)[1].split("\n}",1)[0]
assert "SOF_HDA_SD_CTL_DMA_START, 0" in stop
reset = wave.split("NTSTATUS SofWaveRtResetHdaStream(",1)[1].split("\n}",1)[0]
assert reset.index("SofWaveRtStopHdaStream") < reset.index("SofWaveRtSetHdaControl(regs, SOF_HDA_SD_CTL_DMA_RESET, SOF_HDA_SD_CTL_DMA_RESET)")
assert reset.index("if (NT_SUCCESS(status))") < reset.index("writel(0, regs + SOF_HDA_ADSP_REG_SD_BDLPL)") < reset.index("Stream->RingAllocation.Published = FALSE;")
assert free.index("stopped = NT_SUCCESS(resetStatus)") < free.index("if (stopped)") < free.index("SofFreeDmaBuffer")
assert "NTSTATUS status = ValidateFormat(Format, m_format);" in (root/"src/driver/PortClsAdapter.cpp").read_text()

# Execute the copier serializer with the wire declarations used by the driver.
topo_h = (root/"src/driver/Topo.h").read_text()
abi_h = (root/"include/sof/ipc4/header.h").read_text()
def c_struct(source, name):
    start = source.index("struct "+name+" {")
    end = source.index("\n}",start)+2
    return source[start:end]+";\n"

wire = "#pragma pack(push,1)\n"
for name in ("sof_ipc4_audio_format", "sof_ipc4_base_module_cfg"):
    wire += c_struct(abi_h, name)
for name in ("sof_ipc4_gtw_attributes","sof_copier_gateway_cfg","sof_ipc4_copier_data",
             "sof_ipc4_alh_mapping","sof_ipc4_alh_configuration_blob"):
    wire += c_struct(topo_h,name)
wire += "#pragma pack(pop)\n"
wire += topo_h[topo_h.index("typedef struct _SOF_COPIER_PARAMS {"):
               topo_h.index("/**\n * @struct _SOF_PIPELINE_DESC")]
defines = "\n".join(line for line in topo_h.splitlines() if line.startswith((
    "#define SOF_IPC4_NODE_", "#define SOF_IPC4_GTW_", "#define SOF_IPC4_COPIER_DEFAULT_",
    "#define SOF_IPC4_TYPE_LSB_INTEGER")))
serializer = topo[topo.index("VOID SofIpc4BuildAudioFormat("):topo.index("VOID SofIpc4BuildDefaultStereoFormat(")]
serializer += helper + "NTSTATUS SofIpc4BuildCopierConfig(" + copier + "\n}\n"
copier_test = r'''
int main(void) {
    unsigned char blob[] = {0,0,0,0,0x78,0x56,0x34,0x12};
    UCHAR bytes[128];
    struct sof_ipc4_copier_data *data=(struct sof_ipc4_copier_data*)bytes;
    SOF_COPIER_PARAMS params={0};
    ULONG result=32;
    params.NodeType=SOF_IPC4_GTW_SSP_OUT;
    params.NodeIndex=SOF_IPC4_NODE_INDEX_INTEL_SSP(1);
    params.AudioFormat=(SOF_AUDIO_FORMAT_PARAMS){48000,32,24,2,0,0xffffff10u,1};
    memset(bytes,0xa5,sizeof(bytes));
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_DEVICE_CONFIGURATION_ERROR);
    assert(!result && bytes[0]==0xa5);
    params.GatewayConfig=blob;
    params.GatewayConfigBytes=sizeof(blob)-1u;
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_INVALID_BUFFER_SIZE);
    assert(!result && bytes[0]==0xa5);
    params.GatewayConfigBytes=sizeof(blob);
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(*data),&result)==STATUS_BUFFER_TOO_SMALL);
    assert(!result && bytes[0]==0xa5);
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_SUCCESS);
    assert(result==sizeof(*data)+4u && data->gtw_cfg.node_id==0xc10u);
    assert(data->gtw_cfg.config_length==2u && memcmp(data->gtw_cfg.config_data,blob,sizeof(blob))==0);
    params.NodeType=SOF_IPC4_GTW_SSP_IN;
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_SUCCESS);
    assert(data->gtw_cfg.node_id==0xd10u);
    params.NodeType=SOF_IPC4_GTW_DMIC;
    params.NodeIndex=SOF_IPC4_NODE_INDEX_INTEL_DMIC(1u);
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_SUCCESS);
    assert(data->gtw_cfg.node_id==0xb01u);
    params.NodeType=SOF_IPC4_GTW_ALH_IN;
    params.NodeIndex=4u;
    params.AlhStreamId=4u;
    params.GatewayConfig=NULL;
    params.GatewayConfigBytes=0u;
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_SUCCESS);
    assert(data->gtw_cfg.node_id==0x1104u && data->gtw_cfg.config_length==4u);
    params.NodeType=SOF_IPC4_GTW_ALH_OUT;
    params.NodeIndex=3u;
    params.AlhStreamId=3u;
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_SUCCESS);
    assert(data->gtw_cfg.node_id==0x1003u);
    params.NodeType=2u;
    assert(SofIpc4BuildCopierConfig(&params,bytes,sizeof(bytes),&result)==STATUS_INVALID_PARAMETER);
    return 0;
}
'''
shim = r'''
#include <string.h>
typedef unsigned char UCHAR, BOOLEAN;
typedef void VOID, *PVOID;
typedef SOF_AUDIO_FORMAT_PARAMS *PSOF_AUDIO_FORMAT_PARAMS;
#define FALSE 0
#define TRUE 1
#define STATUS_DEVICE_CONFIGURATION_ERROR (-2)
#define STATUS_INVALID_BUFFER_SIZE (-3)
#define STATUS_BUFFER_TOO_SMALL (-4)
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
#define NT_SUCCESS(status) ((status)>=0)
#define _In_
#define _Out_
#define _Out_writes_bytes_(n)
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))
'''
with tempfile.TemporaryDirectory(prefix="pj_copier_") as tmp:
    test = Path(tmp)/"test.c"
    exe = Path(tmp)/("test.exe" if os.name=="nt" else "test")
    test.write_text(fixture+shim+defines+"\n"+wire+serializer+copier_test)
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",
                    *shlex.split(os.environ.get("CFLAGS","")),str(test),"-o",str(exe)],
                   check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=10)
print("PASS: SSP/DMIC gateway blobs, literal direction encodings and guarded writes")
