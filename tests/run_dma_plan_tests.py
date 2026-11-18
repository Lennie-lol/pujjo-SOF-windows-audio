#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute the pure WDK-linked DMA/PCM planner; no real DMA or device access."""
import os, shutil, subprocess, tempfile
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
    assert(pj_pcm_period(48000u,2u,32u,24u,10u,65536u,&period) && period==3840u);
    assert(!pj_pcm_period(48000u,0u,32u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,16u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(44100u,2u,32u,24u,1u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,32u,24u,10u,256u,&period));
    assert(!pj_pcm_period(48000u,2u,24u,24u,10u,65536u,&period));
    assert(!pj_pcm_period(0xffffffffu,8u,32u,32u,1000u,65536u,&period));
    assert(!pj_pcm_period(48000u,2u,32u,24u,10u,65536u,NULL));
    assert(period==3840u);
    puts("PASS: DMA carry, span overflow, alignment, and checked PCM geometry");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dma_plan_") as tmp:
    test=Path(tmp)/"test.c"
    exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
    test.write_text(main)
    subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-Wconversion",
                    "-Wsign-conversion","-pedantic","-I"+str(root/"src/driver"),
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
assert "!SofWaveRtHardwareBackendVerified()" in wave
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
assert topo.count("SofIpc4BuildGainConfigForFormat(Fmt,")==8
gain=topo.split("static NTSTATUS SofIpc4BuildGainConfigForFormat(",1)[1].split("\n}",1)[0]
assert "SofIpc4BuildAudioFormat(Fmt," in gain
copier=topo.split("NTSTATUS SofIpc4BuildCopierConfig(",1)[1].split("\n}",1)[0]
assert copier.index("totalSize += sizeof(struct sof_ipc4_alh_configuration_blob)") < copier.index("if (MaxBuffer < totalSize)") < copier.index("RtlZeroMemory(CopierBuffer, totalSize)")
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
assert configure.index("if (timeout == 500u || (ctl & SOF_HDA_SD_CTL_DMA_START))") < configure.index("SOF_HDA_ADSP_REG_SD_BDLPL")
print("PASS: code loader BAR preflight and required reset acknowledgment")
