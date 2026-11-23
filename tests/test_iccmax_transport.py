"""Exercise production ICCMAX setup and teardown with host-only MMIO."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[1] / "src/driver"
loader = (driver / "Loader.c").read_text()
functions = loader[loader.index("static NTSTATUS SofLoaderDmaAddressesMatch("):
                   loader.index("static NTSTATUS SofQueryBootedFirmwareVersion(")]
shim = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "PujjoDmaPlan.h"
typedef uint8_t UCHAR, BOOLEAN, *PUCHAR;
typedef uint32_t ULONG;
typedef int NTSTATUS;
#define MAXULONG UINT32_MAX
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_DEVICE_BUSY -2
#define STATUS_DEVICE_CONFIGURATION_ERROR -3
#define STATUS_INVALID_BUFFER_SIZE -4
#define STATUS_IO_TIMEOUT -5
#define STATUS_IO_DEVICE_ERROR -6
#define NT_SUCCESS(s) ((s) >= 0)
#define _In_
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define SOF_HDA_GCTL 8u
#define SOF_HDA_GCTL_RESET 1u
#define SOF_HDA_INTCTL 32u
#define SOF_HDA_PPCTL_GPROCEN (1u<<30)
#define SofLogBreadcrumb(s) ((void)(s))
#define KeMemoryBarrier() ((void)0)
#define PAGE_SIZE 4096u
#define SOF_HDA_ADSP_LOADER_BASE 0x80u
#define SOF_HDA_ADSP_SD_ENTRY_SIZE 32u
#define SOF_HDA_REG_PP_PPCTL 4u
#define SOF_HDA_ADSP_REG_SD_STS 3u
#define SOF_HDA_ADSP_REG_SD_CBL 8u
#define SOF_HDA_ADSP_REG_SD_LVI 12u
#define SOF_HDA_ADSP_REG_SD_BDLPL 24u
#define SOF_HDA_ADSP_REG_SD_BDLPU 28u
#define SOF_HDA_SD_CTL_DMA_START 2u
#define SOF_HDA_SD_CTL_DMA_RESET 1u
#define SOF_HDA_SD_STS_DMA_ERRORS 0x18u
#define SOF_HDA_CL_DMA_SD_INT_MASK 0x1cu
typedef struct { ULONG AddrLow, AddrHigh, Size, Ioc; } BDL, *PSOF_INTEL_DSP_BDL;
typedef struct { void *VirtualAddress; struct {ULONG LowPart, HighPart;} PhysicalAddress; } SOF_DMA_BUFFER;
typedef struct { PUCHAR Bar0VirtualAddress; ULONG Bar0Length, NumCaptureStreams,
    NumPlaybackStreams, PpCapOffset, BootPpBitsAdded, BootIntBitsAdded;
    BOOLEAN Removed, DspNeedsPowerDown, BootDmaStopUnconfirmed, IrqMaskNeeded;
} CTX, *PSOF_DEVICE_CONTEXT;
static int SofDeviceRemoved(CTX *c) { return c->Removed; }
static int pj_ipc4_mmio_span_valid(ULONG length, ULONG offset, ULONG bytes) {
    return offset<=length && bytes<=length-offset;
}
static UCHAR bar[8192];
static BDL storage[2][PAGE_SIZE/sizeof(BDL)];
static unsigned allocs, frees, starts;
static PUCHAR drop;
static ULONG readl(PUCHAR p) { ULONG v; memcpy(&v,p,4); return v; }
static void writel(ULONG v, PUCHAR p) { if (p != drop) memcpy(p,&v,4); }
static unsigned readw(PUCHAR p) { uint16_t v; memcpy(&v,p,2); return v; }
static void writew(uint16_t v, PUCHAR p) { if (p != drop) memcpy(p,&v,2); }
static UCHAR READ_REGISTER_UCHAR(PUCHAR p) { return *p; }
static void WRITE_REGISTER_UCHAR(PUCHAR p, UCHAR v) {
    if (p == drop) return;
    if (p == bar+0x83) *p &= (UCHAR)~v; else *p=v;
}
static ULONG SofCodeLoaderReadControl(PUCHAR p) { return *p; }
static void SofCodeLoaderWriteControl(PUCHAR p, ULONG v) {
    if (v & SOF_HDA_SD_CTL_DMA_START) ++starts;
    if (p != drop) *p=(UCHAR)v;
}
static void KeStallExecutionProcessor(unsigned us) { assert(us == 10); }
static NTSTATUS SofAllocateDmaBuffer(CTX *c, ULONG n, SOF_DMA_BUFFER *b) {
    assert(c && allocs<2 && n==PAGE_SIZE);
    b->VirtualAddress=storage[allocs++]; b->PhysicalAddress.LowPart=allocs*PAGE_SIZE;
    return 0;
}
static void SofFreeDmaBuffer(CTX *c, SOF_DMA_BUFFER *b) {
    assert(c);
    if (b->VirtualAddress) {
        assert(!(bar[0x80]&2) && !readl(bar+0x98) && !readl(bar+0x9c));
        ++frees; b->VirtualAddress=NULL;
    }
}
"""
cases = r"""
int main(void) {
    struct { int offset, start_status, stop_status; } cases[] = {
        {-1,0,0}, {0x88,-1,0}, {0x8c,-1,0}, /* dropped CBL or LVI */
        {0x204,0,-1}, {0x1048,0,-1}, /* lost restoration */
        {0x83,0,-6}, /* late descriptor error */
        {0x80,0,-5}, {0x98,0,-1} /* stuck RUN or attached BDL */
    };
    CTX c={.Bar0VirtualAddress=bar,.Bar0Length=sizeof(bar),.NumCaptureStreams=1,
           .NumPlaybackStreams=1,.PpCapOffset=0x200};
    assert(SofAcquireBootProcessing(&c)==STATUS_DEVICE_NOT_READY); /* controller held in reset */
    writel(1,bar+SOF_HDA_GCTL);
    writel(8,bar+0x204);
    assert(SofAcquireBootProcessing(&c)==0 && c.BootPpBitsAdded==SOF_HDA_PPCTL_GPROCEN);
    assert(readl(bar+0x204)==(8|SOF_HDA_PPCTL_GPROCEN));
    c.BootIntBitsAdded=1u<<31; writel(c.BootIntBitsAdded|2,bar+SOF_HDA_INTCTL);
    c.BootDmaStopUnconfirmed=1;
    assert(SofRestoreBootController(&c)==STATUS_DEVICE_NOT_READY && c.BootIntBitsAdded);
    c.BootDmaStopUnconfirmed=0; drop=bar+0x204;
    assert(SofRestoreBootController(&c)==STATUS_DEVICE_NOT_READY && c.BootPpBitsAdded);
    assert(!c.BootIntBitsAdded && readl(bar+SOF_HDA_INTCTL)==2);
    drop=NULL;
    assert(SofRestoreBootController(&c)==0 && readl(bar+0x204)==8);
    writel(SOF_HDA_PPCTL_GPROCEN,bar+0x204);
    assert(SofAcquireBootProcessing(&c)==0 && !c.BootPpBitsAdded);
    assert(SofRestoreBootController(&c)==0 && readl(bar+0x204)==SOF_HDA_PPCTL_GPROCEN);
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        SOF_ICCMAX_DMA d={0};
        memset(bar,0,sizeof(bar)); drop=NULL; allocs=frees=starts=0;
        bar[0x1048]=0x43; writel(0x40000000u,bar+0x204);
        int offset=cases[i].offset;
        if (cases[i].start_status) drop=bar+offset;
        assert(SofStartIccmaxDma(&c,&d)==cases[i].start_status);
        assert(starts==(unsigned)!cases[i].start_status);
        drop=offset<0 ? NULL : bar+offset;
        if (offset==0x83) bar[offset]=0x10;
        assert(SofStopIccmaxDma(&c,&d)==cases[i].stop_status);
        if (offset==0x80 || offset==0x98) {
            assert(d.Published && frees==0);
            drop=NULL; assert(SofStopIccmaxDma(&c,&d)==0);
        }
        assert(!d.Published && frees==2);
        if (offset!=0x204) assert(readl(bar+0x204)==0x40000000u);
        if (offset!=0x1048) assert(bar[0x1048]==0x43);
    }
    for (unsigned i=0;i<2;++i) {
        SOF_ICCMAX_DMA d={0};
        memset(bar,0,sizeof(bar)); allocs=starts=0; bar[0x83]=i ? 0xff : 0x10;
        assert(SofStartIccmaxDma(&c,&d)==(i ? STATUS_DEVICE_NOT_READY : STATUS_IO_DEVICE_ERROR));
        assert(!allocs && !starts);
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory() as temp:
    src = Path(temp) / "iccmax.c"
    exe = Path(temp) / ("iccmax.exe" if os.name == "nt" else "iccmax")
    restore = loader[loader.index("NTSTATUS SofRestoreBootController("):
                     loader.index("#ifdef PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT\nstatic NTSTATUS SofAcquireBootProcessing")]
    acquire = loader[loader.index("static NTSTATUS SofAcquireBootProcessing("):
                     loader.index("/* Confirm both halves of the descriptor address")]
    src.write_text(shim + restore + acquire + functions + cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", *shlex.split(os.environ.get("CFLAGS", "")),
                    "-I", str(driver), str(src), str(driver / "PujjoDmaPlan.c"),
                    "-o", str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)
print("PASS: ICCMAX descriptor geometry, restoration, error and buffer retention (host MMIO)")
