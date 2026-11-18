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
