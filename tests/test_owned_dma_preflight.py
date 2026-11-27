#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fault inject the actual MDL-backed unpublished DMA implementation."""
from pathlib import Path
import os, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
loader = (root / "src/driver/Loader.c").read_text()
start = loader.index("typedef struct _SOF_MDL_DMA_BUFFER")
end = loader.index("/* Inspect the exact PDO", start)
body = loader[start:end]
guard = loader.index("#ifdef PUJJO_DMIC_DMA_PREFLIGHT", loader.index("static NTSTATUS SofVerifyAdlNIpc4Candidate"))
assert guard < start < end
assert "SofStageOwnedDmicBuffers" in loader[loader.index("NTSTATUS SofRunDmicDmaPreflight"):loader.index("/* Remove only bits raised")]
for forbidden in ("WRITE_REGISTER", "writel(", "MmMapIoSpace", "WdfDmaTransaction", "SofStartFirmwareBoot", "MmGetPhysicalAddress"):
    assert forbidden not in body, forbidden
shim = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PujjoDmaPlan.h"
#include "PujjoDmicProfile.h"
typedef int NTSTATUS, BOOLEAN;
typedef void VOID, *PVOID;
typedef uint32_t ULONG;
typedef unsigned char UCHAR;
typedef union { uint64_t QuadPart; struct { uint32_t LowPart, HighPart; }; } PHYSICAL_ADDRESS;
typedef struct _MDL {
 struct _MDL *Next;
 ULONG bytes, offset, number;
 void *va;
 int mapped, freed;
} MDL, *PMDL;
typedef struct _DMA_ADAPTER *PDMA_ADAPTER;
typedef struct {
 size_t Size;
 void (*FreeCommonBuffer)(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,PVOID,BOOLEAN);
 NTSTATUS (*CreateCommonBufferFromMdl)(PDMA_ADAPTER,PMDL,void*,ULONG,PHYSICAL_ADDRESS*);
} DMA_OPERATIONS;
struct _DMA_ADAPTER { DMA_OPERATIONS *DmaOperations; };
#define PAGE_SIZE 4096u
#define MAXULONG UINT32_MAX
#define MAXLONGLONG INT64_MAX
#define MmCached 1
#define MM_ALLOCATE_FULLY_REQUIRED 4
#define MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS 32
#define NormalPagePriority 16
#define MdlMappingNoExecute 64
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_NOT_SUPPORTED -2
#define STATUS_INSUFFICIENT_RESOURCES -3
#define STATUS_INVALID_BUFFER_SIZE -4
#define STATUS_DEVICE_CONFIGURATION_ERROR -5
#define STATUS_DATA_ERROR -6
#define NT_SUCCESS(s) ((s)>=0)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define ROUND_TO_PAGES(n) (((n)+PAGE_SIZE-1u)&~(PAGE_SIZE-1u))
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory memcpy
#define RtlStringCbPrintfA snprintf
#define SofLogBreadcrumb(s) ((void)(s))
static unsigned failAt, fault, attempts, totalPages, totalMapped, released, unmapped;
static unsigned count, compareFault, test64;
static MDL *live[6];
static PMDL MmAllocatePagesForMdlEx(PHYSICAL_ADDRESS lo,PHYSICAL_ADDRESS hi,
 PHYSICAL_ADDRESS skip,ULONG n,int cache,int flags) {
 ++attempts;
 assert(!lo.QuadPart && !skip.QuadPart && hi.QuadPart==(test64?INT64_MAX:UINT32_MAX));
 assert(cache==MmCached && flags==(MM_ALLOCATE_FULLY_REQUIRED|MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS));
 assert(n && n%PAGE_SIZE==0);
 if(attempts==failAt && fault==1) return NULL;
 MDL *m=calloc(1,sizeof(*m)); assert(m && count<6);
 m->bytes=n; m->number=attempts;
 if(attempts==failAt && fault==2) m->bytes-=PAGE_SIZE;
 if(attempts==failAt && fault==3) m->offset=1;
 if(attempts==failAt && fault==4) m->Next=m;
 live[count++]=m; ++totalPages;
 return m;
}
static ULONG MmGetMdlByteCount(MDL *m) {return m->bytes;}
static ULONG MmGetMdlByteOffset(MDL *m) {return m->offset;}
static void *MmGetSystemAddressForMdlSafe(MDL *m,int flags) {
 assert(flags==(NormalPagePriority|MdlMappingNoExecute));
 if(m->number==failAt && fault==5) return NULL;
 m->va=malloc(m->bytes); assert(m->va); return m->va;
}
static uint64_t address(unsigned number) {
 if(number==failAt && fault==7) return 0;
 if(number==failAt && fault==8) return 0x100001;
 if(number==failAt && fault==9) return 0xfffff000;
 if(number==failAt && fault==10) return UINT64_MAX-4095;
 return (test64?0x100000000ULL:0)+number*0x100000ULL;
}
static NTSTATUS create(PDMA_ADAPTER a,MDL *m,void *configs,ULONG n,PHYSICAL_ADDRESS *out) {
 assert(a && m->va && !configs && !n && !m->mapped);
 if(m->number==failAt && fault==6) return -77;
 out->QuadPart=address(m->number); m->mapped=1; ++totalMapped; return 0;
}
static void unmap(PDMA_ADAPTER a,ULONG n,PHYSICAL_ADDRESS logical,void *va,BOOLEAN cache) {
 assert(a && count && cache==TRUE);
 MDL *m=live[count-1];
 assert(m->mapped && n==m->bytes && va==m->va && logical.QuadPart==address(m->number));
 m->mapped=0; ++unmapped;
}
static void MmFreePagesFromMdl(MDL *m) {
 assert(count && live[count-1]==m && !m->mapped && !m->freed);
 free(m->va); m->freed=1; ++released;
}
static void ExFreePool(MDL *m) {
 assert(m->freed && live[count-1]==m);
 --count; free(m);
}
static size_t RtlCompareMemory(void *a,const void *b,size_t n) {
 return !compareFault && !memcmp(a,b,n)?n:0;
}
'''
cases = r'''
static void reset(void) {
 assert(!count);
 attempts=totalPages=totalMapped=released=unmapped=failAt=fault=compareFault=0;
}
static void check(void) {
 assert(!count && totalPages==released && totalMapped==unmapped);
}
int main(void) {
 DMA_OPERATIONS ops={sizeof(ops),unmap,create};
 struct _DMA_ADAPTER adapter={&ops};
 UCHAR *payload=malloc(700000); assert(payload); memset(payload,0xab,700000);
 for(test64=0;test64<2;++test64) {
  reset();
  assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)==0);
  assert(attempts==6 && totalMapped==6); check();
  for(unsigned k=1;k<=6;++k) {
   for(unsigned f=1;f<=10;++f) {
    /* One-page span at the address-width boundary is legal. */
    if((f==9 && (test64||k==2||k==3||k==4||k==6)) ||
       (f==10 && (k==2||k==3||k==4||k==6) && test64)) continue;
    reset(); failAt=k; fault=f;
    assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)<0);
    assert(attempts==k); check();
   }
  }
  reset(); compareFault=1;
  assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)==STATUS_DATA_ERROR); check();
  reset();
  assert(SofStageOwnedDmicBuffers(NULL,test64,payload,700000)==STATUS_NOT_SUPPORTED); check();
  ops.Size=offsetof(DMA_OPERATIONS,CreateCommonBufferFromMdl);
  assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)==STATUS_NOT_SUPPORTED); check();
  ops.Size=sizeof(ops); ops.CreateCommonBufferFromMdl=NULL;
  assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)==STATUS_NOT_SUPPORTED); check();
  ops.CreateCommonBufferFromMdl=create;
  ops.FreeCommonBuffer=NULL;
  assert(SofStageOwnedDmicBuffers(&adapter,test64,payload,700000)==STATUS_NOT_SUPPORTED); check();
  ops.FreeCommonBuffer=unmap;
  assert(!attempts);
 }
 free(payload);
 puts("PASS: MDL-backed common buffers; both address widths; missing APIs; injected partial allocation, mapping failure, invalid logical spans; reverse unmap-before-page-free");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_mdl_dma_") as d:
    c=Path(d)/"test.c"; exe=Path(d)/("test.exe" if os.name=="nt" else "test")
    c.write_text(shim+body+cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),"-I"+str(root/"src/driver"),
                    str(c),str(root/"src/driver/PujjoDmaPlan.c"),"-o",str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=20)
