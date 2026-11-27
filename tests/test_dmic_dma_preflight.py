#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise the production unpublished staging body using fault-injected host allocations."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
loader = (root / "src/driver/Loader.c").read_text()
device = (root / "src/driver/Device.c").read_text()
start = loader.index("static NTSTATUS SofStageUnpublishedDmicBuffers(")
end = loader.index("\n/* This callback is reached", start)
stage = loader[start:end]
orchestration = loader[end:loader.index("\n/* Remove only bits raised", end)]
for forbidden in ("writel(", "writew(", "WRITE_REGISTER_", "READ_REGISTER_", "MmMapIoSpace(",
                  "WdfInterruptCreate(", "SofStartFirmwareBoot(", "WdfDmaTransaction", "SofPublishAudioChild"):
    assert forbidden not in stage + orchestration, forbidden
assert "Ctx->Bar0VirtualAddress || Ctx->Bar4VirtualAddress" in orchestration
assert orchestration.index("SofVerifyAdlNIpc4Candidate") < orchestration.index("SofParseFirmwareImage") < orchestration.index("WdfDmaEnablerCreate")
assert orchestration.index("pj_firmware_plan_ll") < orchestration.index("WdfDmaEnablerCreate") < orchestration.index("SofStageUnpublishedDmicBuffers") < orchestration.index("WdfObjectDelete")
assert "STATUS_NOT_FOUND || status == STATUS_OBJECT_NAME_NOT_FOUND" in orchestration
prepare = device.split("A physical read-only pre-DMA milestone",1)[1].split("#else",1)[0]
assert prepare.index("SofUnmapPreparedBars(ctx)") < prepare.index("SofRunDmicDmaPreflight(ctx)")
assert "PUJJO_DMIC_DMA_PREFLIGHT" in prepare
src = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PujjoDmicProfile.h"
#include "PujjoDmaPlan.h"
typedef uint32_t ULONG;
typedef unsigned char UCHAR;
typedef int NTSTATUS;
typedef struct { int unused; } CTX, *PSOF_DEVICE_CONTEXT;
typedef union {
    uint64_t QuadPart;
    struct { uint32_t LowPart, HighPart; };
} ADDRESS;
typedef struct { void *VirtualAddress; ADDRESS PhysicalAddress; } SOF_DMA_BUFFER;
#define STATUS_INVALID_BUFFER_SIZE (-1)
#define STATUS_DEVICE_CONFIGURATION_ERROR (-2)
#define STATUS_DATA_ERROR (-3)
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlCopyMemory memcpy
#define RtlStringCbPrintfA snprintf
static unsigned attempts, live, released, failAt, zeroAt, corruptAt, compareFail;
static unsigned order[6];
static size_t RtlCompareMemory(const void *a,const void *b,size_t n) {
    return !compareFail && !memcmp(a,b,n) ? n : 0;
}
static void SofLogBreadcrumb(const char *msg) { (void)msg; }
static NTSTATUS SofAllocateDmaBuffer(CTX *ctx, ULONG n, SOF_DMA_BUFFER *b) {
    (void)ctx;
    ++attempts;
    if (attempts == failAt) return -100;
    assert(live<6);
    assert(n==(attempts==1?700000u:attempts==5?15360u:4096u));
    b->VirtualAddress=calloc(n,1);
    assert(b->VirtualAddress);
    b->PhysicalAddress.QuadPart=attempts==zeroAt ? 0 : 0x100000000ULL + attempts*0x100000ULL;
    if(attempts==corruptAt) ++b->PhysicalAddress.QuadPart;
    order[live++]=attempts;
    return 0;
}
static void SofFreeDmaBuffer(CTX *ctx, SOF_DMA_BUFFER *b) {
    (void)ctx;
    assert(live && b->VirtualAddress);
    if(b->PhysicalAddress.QuadPart)
        assert((b->PhysicalAddress.QuadPart-0x100000000ULL)/0x100000ULL==order[live-1]);
    free(b->VirtualAddress); --live; ++released;
}
''' + stage + r'''
static void reset(void) { attempts=live=released=failAt=zeroAt=corruptAt=compareFail=0; }
int main(void) {
    CTX ctx={0};
    UCHAR *payload=malloc(700000);
    assert(payload);
    memset(payload,0x5a,700000);
    reset();
    assert(SofStageUnpublishedDmicBuffers(&ctx,payload,700000)==0);
    assert(attempts==6 && released==6 && !live);
    for(unsigned i=1;i<=6;++i) {
        reset(); failAt=i;
        assert(SofStageUnpublishedDmicBuffers(&ctx,payload,700000)==-100);
        assert(attempts==i && released==i-1 && !live);
        reset(); zeroAt=i;
        assert(SofStageUnpublishedDmicBuffers(&ctx,payload,700000)==STATUS_DEVICE_CONFIGURATION_ERROR);
        assert(attempts==i && released==i && !live);
    }
    for(unsigned i=1;i<=5;i+=2) {
        reset(); corruptAt=i;
        assert(SofStageUnpublishedDmicBuffers(&ctx,payload,700000)==STATUS_INVALID_BUFFER_SIZE);
        assert(released==6 && !live);
    }
    reset(); compareFail=1;
    assert(SofStageUnpublishedDmicBuffers(&ctx,payload,700000)==STATUS_DATA_ERROR);
    assert(released==6 && !live);
    reset();
    assert(SofStageUnpublishedDmicBuffers(&ctx,NULL,700000)==STATUS_INVALID_BUFFER_SIZE);
    assert(SofStageUnpublishedDmicBuffers(&ctx,payload,255)==STATUS_INVALID_BUFFER_SIZE);
    assert(!attempts && !live);
    free(payload);
    puts("PASS: six simultaneous unpublished buffers, geometry and payload verification, reverse cleanup on all injected failures");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dma_preflight_") as d:
    c=Path(d)/"test.c"; exe=Path(d)/("test.exe" if os.name=="nt" else "test")
    c.write_text(src)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),"-I"+str(root/"src/driver"),
                    str(c),str(root/"src/driver/PujjoDmaPlan.c"),"-o",str(exe)],check=True,timeout=45)
    subprocess.run([str(exe)],check=True,timeout=20)
