#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute the production NHLT graph control flow against fake IPC operations.
These fixtures do not execute DSP firmware, hardware I/O or WDK services.
"""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "src/driver/Topo.c").read_text()
graph = source[source.index("static NTSTATUS SofIpc4CreateNhltPipeline("):
               source.index("NTSTATUS SofIpc4CreateSspPipeline(")]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define _KERNEL_MODE
typedef uint32_t ULONG, PJ_DWORD;
typedef uint16_t USHORT;
typedef unsigned char UCHAR, BOOLEAN;
typedef ULONG *PULONG;
typedef int NTSTATUS;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_NOT_SUPPORTED -2
#define STATUS_DEVICE_NOT_READY -3
#define STATUS_BUFFER_TOO_SMALL -4
#define STATUS_INVALID_BUFFER_SIZE -5
#define STATUS_INSUFFICIENT_RESOURCES -6
#define STATUS_TEST_FAILURE -7
#define NT_SUCCESS(s) ((s)>=0)
#define SOF_FW_BOOT_COMPLETE 1
#define NHLT_LINK_DMIC 2
#define NHLT_LINK_SSP 3
#define NHLT_DEVICE_DMIC 1
#define NHLT_DEVICE_I2S 4
#define NHLT_DIRECTION_CAPTURE 1
#define NHLT_DIRECTION_RENDER 0
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
#define PJ_CATALOG_OK 0
#define PJ_DMIC_RATE 48000u
#define PJ_DMIC_CHANNELS 4u
#define PJ_DMIC_BITS 32u
typedef struct { ULONG bytes; } pj_dmic_profile;
static int bad_dmic;
static int pj_dmic_validate(const UCHAR *p,ULONG n,pj_dmic_profile *profile) {
    assert(p && n==96 && profile);return !bad_dmic;
}
#define POOL_FLAG_NON_PAGED 0
#define SOF_DEVICE_TAG 0
#define SOF_IPC4_GTW_DMIC 12
#define SOF_IPC4_GTW_SSP_IN 13
#define SOF_IPC4_GTW_SSP_OUT 14
#define SOF_IPC4_GTW_HOST_IN 15
#define SOF_IPC4_GTW_HOST 16
#define SOF_IPC4_NODE_INDEX_INTEL_DMIC(n) (n)
#define SOF_IPC4_NODE_INDEX_INTEL_SSP(n) ((n)<<4)
#define SOF_IPC4_VOLUME_MUTE 0
#define SOF_IPC4_MOD_ID_COPIER 1
#define SOF_IPC4_MOD_ID_GAIN 2
#define SOF_IPC4_PIPE_PAUSED 3
typedef struct { ULONG SamplingFrequency, ChannelsCount, BitDepth, ValidBitDepth; } SOF_AUDIO_FORMAT_PARAMS;
typedef struct { ULONG LinkType,DeviceType,VirtualBusId,Direction,SampleRate,Channels,ContainerBits,ValidBits; } SOF_NHLT_CONFIG_KEY;
typedef struct { ULONG NodeType,NodeIndex; SOF_AUDIO_FORMAT_PARAMS AudioFormat; UCHAR *GatewayConfig; ULONG GatewayConfigBytes; } SOF_COPIER_PARAMS;
typedef struct { ULONG unused; } pj_firmware_config;
struct sof_ipc4_copier_data { ULONG words[16]; };
struct sof_ipc4_gain_data { ULONG words[8]; };
typedef struct { void *NhltTable; ULONG NhltTableSize; int AudioDmaFaulted,FwState,CopierProfile,GainProfile; } DEVICE, *PSOF_DEVICE_CONTEXT;
static int fail_at, step, delete_fails, deletes, frees, allocations, active_memory;
static int bad_length, fail_lookup, fail_plan, copier_count, bind_count;
static ULONG copies[2], nodes[2], bound[2][4];
static SOF_NHLT_CONFIG_KEY observed;
static int tick(void) { return ++step==fail_at ? STATUS_TEST_FAILURE : STATUS_SUCCESS; }
static void *ExAllocatePool2(int flags, size_t size, int tag) {
    (void)flags;(void)tag; ++active_memory; return malloc(size);
}
static void ExFreePoolWithTag(void *p,int tag) { (void)tag; --active_memory;free(p); }
static int SofIpc4ComputeChunkBytes(const SOF_AUDIO_FORMAT_PARAMS *f, ULONG *b) {
    *b=f->SamplingFrequency/1000*f->ChannelsCount*(f->BitDepth/8);
    return *b ? 0 : -1;
}
static int SofNhltCopyConfiguration(void *table,ULONG size,SOF_NHLT_CONFIG_KEY *key,UCHAR *dest,ULONG cap,ULONG *bytes) {
    assert(table && size); observed=*key;
    if(fail_lookup) return STATUS_TEST_FAILURE;
    *bytes=bad_length?95:96;
    if(!dest) return STATUS_BUFFER_TOO_SMALL;
    assert(cap>=*bytes); memset(dest,0x5a,*bytes);return 0;
}
static int pj_firmware_plan_ll(int *p,ULONG ibs,ULONG obs,pj_firmware_config *c,PJ_DWORD *pages) {
    (void)p;(void)c;assert(ibs==obs);*pages=2;return fail_plan?1:0;
}
static int SofIpc4BuildCopierConfig(SOF_COPIER_PARAMS *p,UCHAR *b,ULONG size,ULONG *out) {
    assert(copier_count<2 && size>=4);copies[copier_count]=p->NodeType;nodes[copier_count++]=p->NodeIndex;
    if(p->GatewayConfig) { assert(p->GatewayConfigBytes==96);assert(p->GatewayConfig[95]==0x5a); }
    else assert(!p->GatewayConfigBytes);
    memset(b,0,4);*out=4;return 0;
}
static int SofIpc4BuildGainConfigForFormat(const SOF_AUDIO_FORMAT_PARAMS *f,ULONG gain,UCHAR *b,ULONG size,ULONG *out) {
    assert(f && gain==0 && size>=4);memset(b,0,4);*out=4;return 0;
}
static int SofIpc4CreatePipeline(DEVICE *d,ULONG id,ULONG priority,ULONG pages,ULONG core,ULONG lp) {
    assert(d && id==4 && !priority && pages==6 && !core && !lp);return tick();
}
static int SofIpc4AllocInstanceId(DEVICE *d,ULONG module,ULONG *out) {
    assert(d && (module==1||module==2));int status=tick();
    if(!status) *out=(ULONG)++allocations;
    return status;
}
static int SofIpc4InitModuleInstance(DEVICE *d,ULONG mod,ULONG instance,ULONG id,ULONG core,UCHAR *b,ULONG size) {
    assert(d && (mod==1||mod==2) && instance && id==4 && !core && b && size==4);return tick();
}
static int SofIpc4BindModules(DEVICE *d,ULONG sm,ULONG si,ULONG sq,ULONG dm,ULONG di,ULONG dq) {
    assert(d && !sq && !dq && bind_count<2);bound[bind_count][0]=sm;bound[bind_count][1]=si;
    bound[bind_count][2]=dm;bound[bind_count++][3]=di;return tick();
}
static int SofIpc4SetPipelineState(DEVICE *d,ULONG id,ULONG state) {
    assert(d && id==4 && state==3);return tick();
}
static int SofIpc4DeletePipeline(DEVICE *d,ULONG id) {
    assert(d && id==4); ++deletes;return delete_fails?STATUS_TEST_FAILURE:0;
}
static void SofIpc4FreeInstanceId(DEVICE *d,ULONG mod,ULONG instance) {
    assert(d && (mod==1||mod==2));if(instance!=0xffffffffu)++frees;
}
'''
suffix = r'''
static void reset(DEVICE *d) {
    memset(d,0,sizeof(*d));d->NhltTable=d;d->NhltTableSize=1;d->FwState=1;
    fail_at=step=delete_fails=deletes=frees=allocations=active_memory=0;
    bad_length=fail_lookup=fail_plan=copier_count=bind_count=bad_dmic=0;
}
static int call(DEVICE *d,ULONG bus,BOOLEAN dmic,BOOLEAN capture,SOF_AUDIO_FORMAT_PARAMS *fmt,ULONG *h,ULONG *g,ULONG *a) {
    *h=*g=*a=0;
    return SofIpc4CreateNhltPipeline(d,4,2,bus,dmic,capture,fmt,h,g,a);
}
int main(void) {
    DEVICE d; ULONG h,g,a;
    SOF_AUDIO_FORMAT_PARAMS fmt={48000,2,32,24};
    for(unsigned route=0;route<4;++route) {
        BOOLEAN dmic=route==3,capture=route==1||dmic;
        ULONG bus=route==2?1:0;
        fmt.ChannelsCount=dmic?4u:2u; fmt.ValidBitDepth=dmic?32u:24u;
        reset(&d);
        assert(call(&d,bus,dmic,capture,&fmt,&h,&g,&a)==0);
        assert(h==1 && g==2 && a==3 && step==10 && !deletes && !active_memory);
        assert(observed.VirtualBusId==bus && observed.Direction==capture);
        assert(observed.LinkType==(dmic?2u:3u) && observed.DeviceType==(dmic?1u:4u));
        assert(observed.SampleRate==48000 && observed.Channels==(dmic?4u:2u) && observed.ContainerBits==32 && observed.ValidBits==(dmic?32u:24u));
        assert(copies[0]==(dmic?12u:capture?13u:14u) && nodes[0]==(bus<<4));
        assert(copies[1]==(capture?15u:16u) && nodes[1]==1);
        assert(bound[0][0]==1 && bound[0][1]==(capture?a:h) && bound[0][2]==2 && bound[0][3]==g);
        assert(bound[1][0]==2 && bound[1][1]==g && bound[1][2]==1 && bound[1][3]==(capture?h:a));
        for(int failure=1; failure<=10; ++failure) {
            for(int cleanup_failure=0; cleanup_failure<=1; ++cleanup_failure) {
                reset(&d);fail_at=failure;delete_fails=cleanup_failure;
                assert(call(&d,bus,dmic,capture,&fmt,&h,&g,&a)==STATUS_TEST_FAILURE);
                assert(h==0xffffffffu && g==0xffffffffu && a==0xffffffffu && !active_memory);
                if(failure==1) assert(!deletes && !frees && d.AudioDmaFaulted);
                else {
                    assert(deletes==1 && d.AudioDmaFaulted==cleanup_failure);
                    assert(frees==(cleanup_failure?0:allocations));
                }
            }
        }
    }
    reset(&d);bad_dmic=1;
    assert(call(&d,0,1,1,&fmt,&h,&g,&a)==STATUS_INVALID_PARAMETER && !step && !active_memory);
    fmt.ChannelsCount=2u; fmt.ValidBitDepth=24u;
    reset(&d);assert(call(&d,0,1,1,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);
    reset(&d);assert(call(&d,1,0,1,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);
    assert(call(&d,2,0,0,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);
    assert(call(&d,1,1,1,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);
    fmt.ValidBitDepth=32;assert(call(&d,0,0,0,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);fmt.ValidBitDepth=24;
    bad_length=1;assert(call(&d,0,0,0,&fmt,&h,&g,&a)==STATUS_INVALID_BUFFER_SIZE && !step);
    bad_length=0;fail_plan=1;assert(call(&d,0,0,0,&fmt,&h,&g,&a)==STATUS_NOT_SUPPORTED && !step);
    fail_plan=0;fail_lookup=1;assert(call(&d,0,0,0,&fmt,&h,&g,&a)==STATUS_TEST_FAILURE && !step);
    fail_lookup=0;d.AudioDmaFaulted=1;assert(call(&d,0,0,0,&fmt,&h,&g,&a)==STATUS_DEVICE_NOT_READY && !step);
    puts("PASS: production NHLT graph routes, IPC failure rollback, instance ownership, fail-closed CREATE");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_nhlt_graph_") as temp:
    src = Path(temp) / "graph.c"
    exe = Path(temp) / ("graph.exe" if os.name == "nt" else "graph")
    src.write_text(prefix + graph + suffix)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)],
                   check=True, timeout=40)
    subprocess.run([str(exe)], check=True, timeout=15)
