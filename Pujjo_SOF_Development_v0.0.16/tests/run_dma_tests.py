#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise production DMA allocation body through host WDF doubles, no device DMA."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/driver/Loader.c').read_text();a=s.index('NTSTATUS\nSofAllocateDmaBuffer(');b=s.index('\n}',a)+2
body=s[a:b]
shim=r'''
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef int NTSTATUS;
typedef uint32_t ULONG;
typedef struct {uint64_t QuadPart;} ADDRESS;
typedef struct {void *DmaEnabler;} CONTEXT,*PSOF_DEVICE_CONTEXT;
typedef struct {ULONG Size;void *VirtualAddress;void *CommonBuffer;ADDRESS PhysicalAddress;} SOF_DMA_BUFFER,*PSOF_DMA_BUFFER;
#define _In_
#define _Out_
#define STATUS_INVALID_PARAMETER -1
#define STATUS_INVALID_DEVICE_STATE -2
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#define RtlZeroMemory(p,n) memset(p,0,n)
static int allocation_status;
static unsigned allocations;
static NTSTATUS WdfCommonBufferCreate(void *enabler,ULONG size,void *attrs,void **out) {
 assert(enabler);(void)attrs;++allocations;
 if(allocation_status)return allocation_status;
 *out=malloc(size);assert(*out);memset(*out,0xa5,size);return 0;
}
static void *WdfCommonBufferGetAlignedVirtualAddress(void *buffer) {return buffer;}
static ADDRESS WdfCommonBufferGetAlignedLogicalAddress(void *buffer) {
 ADDRESS address={UINT64_C(0x12345678000)};assert(buffer);return address;
}
'''
main=r'''
int main(void) {
 CONTEXT ctx={0};SOF_DMA_BUFFER b={0};
 assert(SofAllocateDmaBuffer(NULL,64,&b)==STATUS_INVALID_PARAMETER);
 assert(SofAllocateDmaBuffer(&ctx,0,&b)==STATUS_INVALID_PARAMETER);
 assert(SofAllocateDmaBuffer(&ctx,64,NULL)==STATUS_INVALID_PARAMETER);
 assert(SofAllocateDmaBuffer(&ctx,64,&b)==STATUS_INVALID_DEVICE_STATE);
 assert(!allocations && !b.Size && !b.VirtualAddress && !b.CommonBuffer);
 ctx.DmaEnabler=&ctx;allocation_status=-100;
 assert(SofAllocateDmaBuffer(&ctx,64,&b)==-100);
 assert(allocations==1 && !b.Size && !b.VirtualAddress && !b.CommonBuffer);
 allocation_status=0;
 assert(SofAllocateDmaBuffer(&ctx,64,&b)==STATUS_SUCCESS);
 assert(b.Size==64 && b.CommonBuffer==b.VirtualAddress);
 assert(b.PhysicalAddress.QuadPart==UINT64_C(0x12345678000));
 for(unsigned i=0;i<64;++i)assert(!((unsigned char*)b.VirtualAddress)[i]);
 free(b.CommonBuffer);
 puts("PASS: production WDF-only DMA allocation, missing enabler, allocation error and logical address");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pj_dma_') as d:
 src=Path(d)/'dma.c';exe=Path(d)/('dma.exe' if os.name=='nt' else 'dma');src.write_text(shim+body+main)
 cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('CFLAGS','')),str(src),'-o',str(exe)]
 print(' '.join(cmd),flush=True);subprocess.run(cmd,check=True,timeout=120);subprocess.run([str(exe)],check=True,timeout=30)
