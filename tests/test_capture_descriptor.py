"""Run the WaveRT register programming paths against an instrumented HDA descriptor."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from dma_publication_fixture import ledger

root = Path(__file__).resolve().parents[1]
source = (root / "src/driver/WaveRt.c").read_text()
functions = []
for name in ("static NTSTATUS SofWaveRtSetHdaControl", "NTSTATUS SofWaveRtConfigureHdaStream",
             "NTSTATUS SofWaveRtStopHdaStream", "NTSTATUS SofWaveRtResetHdaStream"):
    start = source.index(name + "(")
    functions.append(source[start:source.index("\n}", start) + 2])

fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define _KERNEL_MODE
#define _In_
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define STATUS_DEVICE_CONFIGURATION_ERROR -5
#define STATUS_DEVICE_BUSY -4
#define STATUS_NOT_SUPPORTED -3
#define STATUS_INVALID_BUFFER_SIZE -2
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_IO_TIMEOUT -6
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define SOF_HDA_ADSP_REG_SD_CTL 0
#define SOF_HDA_ADSP_REG_SD_STS 3
#define SOF_HDA_ADSP_REG_SD_CBL 8
#define SOF_HDA_ADSP_REG_SD_LVI 12
#define SOF_HDA_ADSP_REG_SD_FORMAT 18
#define SOF_HDA_ADSP_REG_SD_BDLPL 24
#define SOF_HDA_ADSP_REG_SD_BDLPU 28
#define SOF_HDA_SD_CTL_DMA_START 2u
#define SOF_HDA_SD_CTL_DMA_RESET 1u
#define SOF_HDA_REG_PP_PPCTL 4
#define SOF_HDA_PPCTL_GPROCEN (1u<<30)
#define SOF_HDA_PERIOD_COMPLETE 4
#define SOF_HDA_SD_STS_DMA_ERRORS 24
#define KeMemoryBarrier() ((void)0)
#define KeStallExecutionProcessor(x) ((void)(x))
typedef int NTSTATUS, BOOLEAN;
#define VOID void
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef unsigned char *PUCHAR;
typedef struct { PUCHAR Bar0VirtualAddress; ULONG Bar0Length, PpCapOffset; int removed, HardwarePrepared, BootDmaStopUnconfirmed, AudioDmaFaulted; volatile int32_t DmaPublicationState; } Device,*PSOF_DEVICE_CONTEXT;
typedef struct { Device *DeviceContext; int DescriptorReserved,DescriptorPublished;
 ULONG BdlPeriodCount, StreamIndex, BufferSizeBytes, StreamTag;
 struct {ULONG SamplingFrequency,ValidBitDepth,ChannelsCount;} Format;
 struct {ULONG LowPart,HighPart;} BdlPhysicalAddress;
 struct {int Published;} RingAllocation,BdlAllocation;
 int StreamProcessingAdded;
} Stream,*PSOF_WAVERT_STREAM;
static unsigned char bar[256];
static PUCHAR drop;
static int writes, late_fault;
static int SofDeviceRemoved(Device *d) {return d->removed;}
static PUCHAR SofWaveRtHdaDescriptor(Stream *s) {return s->DeviceContext->removed ? NULL : bar;}
#define SofHdaComputeFormatWord(rate,bits,channels) 0x43u
static ULONG readl(PUCHAR p) {
 ULONG v; memcpy(&v,p,4);
 if(late_fault && p==bar+28 && !v) bar[0]=2;
 return v;
}
static USHORT readw(PUCHAR p) {USHORT v; memcpy(&v,p,2); return v;}
static void writel(ULONG v,PUCHAR p) {++writes; if(p!=drop) memcpy(p,&v,4);}
static void writew(USHORT v,PUCHAR p) {++writes; if(p!=drop) memcpy(p,&v,2);}
#define WRITE_REGISTER_UCHAR(p,v) (*(p)=0)
"""
cases = r"""
int main(void) {
 Device d={.Bar0VirtualAddress=bar,.Bar0Length=sizeof(bar),.PpCapOffset=128,.HardwarePrepared=1};
 assert(SofOpenDmaPublicationGate(&d));
 Stream s={&d,1,0,2,0,15360,1,{48000,32,4},{4096,1},{0},{0},0};
 int offsets[]={8,12,18,24,28};
 assert(SofWaveRtConfigureHdaStream(&s)==STATUS_DEVICE_NOT_READY && !writes);
 writel(SOF_HDA_PPCTL_GPROCEN|1u,bar+132);
 writes=0;
 assert(SofWaveRtConfigureHdaStream(&s)==STATUS_DEVICE_BUSY && !writes);
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
   writel(SOF_HDA_PPCTL_GPROCEN|4u,bar+132);
   drop=bar+offsets[i];
   assert(SofWaveRtConfigureHdaStream(&s)==STATUS_DEVICE_CONFIGURATION_ERROR);
   assert(s.DescriptorPublished && !(bar[0]&2));
   assert(s.RingAllocation.Published && s.BdlAllocation.Published);
   drop=NULL;
   if(offsets[i]==24 || offsets[i]==28) {
     writes=0;
     assert(SofWaveRtResetHdaStream(&s)<0 && s.DescriptorPublished && !writes);
     writel(4096,bar+24); writel(1,bar+28);
   }
   assert(SofWaveRtResetHdaStream(&s)==0 && !s.DescriptorPublished);
   assert(readl(bar+132)==(SOF_HDA_PPCTL_GPROCEN|4u));
   assert(!s.RingAllocation.Published && !s.BdlAllocation.Published && !SofDmaPublishedSlots(&d));
   memset(bar,0,sizeof(bar));
 }
 writel(SOF_HDA_PPCTL_GPROCEN,bar+132);
 bar[0]=2; writes=0;
 assert(SofWaveRtConfigureHdaStream(&s)==STATUS_DEVICE_BUSY);
 assert(SofWaveRtStopHdaStream(&s)==0 && SofWaveRtResetHdaStream(&s)==0 && !writes);
 bar[0]=0;
 assert(SofWaveRtConfigureHdaStream(&s)==0 && s.DescriptorPublished);
 late_fault=1;
 assert(SofWaveRtResetHdaStream(&s)<0 && s.DescriptorPublished);
 late_fault=0; d.removed=1; writes=0;
 assert(SofWaveRtResetHdaStream(&s)<0 && s.DescriptorPublished && !writes);
 d.removed=0;
 bar[0]=0; /* zero-detached descriptor must also be idle */
 drop=bar+132;
 assert(SofWaveRtResetHdaStream(&s)<0 && s.DescriptorPublished && SofDmaPublishedSlots(&d)==1);
 drop=NULL;
 assert(SofWaveRtResetHdaStream(&s)==0 && !s.DescriptorPublished);
 memset(bar,0,sizeof(bar));
 writel(SOF_HDA_PPCTL_GPROCEN,bar+132);
 assert(SofWaveRtConfigureHdaStream(&s)==0);
 SofInvalidateDmaPublicationGate(&d); /* same CAS as surprise removal */
 writes=0;
 /* Even if the bus remains readable, no retirement of its DMA lease. */
 assert(SofWaveRtResetHdaStream(&s)<0 && s.DescriptorPublished);
 assert(!writes && SofDmaPublishedSlots(&d)==1 && s.RingAllocation.Published);
 assert(!SofOpenDmaPublicationGate(&d));
}
"""
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "descriptor.c"
    path.write_text("\n".join([fixture, ledger, *functions, cases]))
    binary = path.with_suffix("")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")), str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
