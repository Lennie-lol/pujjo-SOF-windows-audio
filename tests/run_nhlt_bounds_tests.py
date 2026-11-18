#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host execution of the actual NHLT parser with packed ABI-shaped test types."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / "src/driver/Nhlt.h").read_text()
source = (root / "src/driver/Nhlt.c").read_text()
consts = header[header.index("#define NHLT_ACPI_SIGNATURE"):header.index("#include <pshpack1.h>")]
packed = header[header.index("typedef struct _ACPI_NHLT_HEADER"):header.index("#include <poppack.h>")]
info = header[header.index("typedef struct _SOF_NHLT_DMIC_INFO"):header.index("/* NHLT Parser Functions */")]
body = source[source.index("NTSTATUS SofNhltParse("):source.index("\nNTSTATUS SofNhltQueryAcpiTable(")]
shim = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
typedef uint8_t BOOLEAN;
typedef int NTSTATUS;
#define TRUE 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_BUFFER_TOO_SMALL -2
#define STATUS_NOT_FOUND -3
#define _In_reads_bytes_(n)
#define _In_
#define _Out_
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))
#pragma pack(push,1)
typedef struct {
    struct {USHORT wFormatTag,nChannels; ULONG nSamplesPerSec,nAvgBytesPerSec;
            USHORT nBlockAlign,wBitsPerSample,cbSize;} Format;
    USHORT samples;
    ULONG channelMask;
    UCHAR guid[16];
} WAVEFORMATEXTENSIBLE;
'''
main = r'''
static void fix_checksum(UCHAR *buf, ULONG size) {
    ULONG i; UCHAR sum=0;
    ACPI_NHLT_HEADER *h=(ACPI_NHLT_HEADER*)buf;
    h->Length=size;h->Checksum=0;
    for(i=0;i<size;++i) sum=(UCHAR)(sum+buf[i]);
    h->Checksum=(UCHAR)(0u-sum);
}
static ULONG make_ep(UCHAR *p,UCHAR link,UCHAR count) {
    NHLT_ENDPOINT_DESCRIPTOR ep={0};
    ULONG cap=0;
    WAVEFORMATEXTENSIBLE fmt={0};
    ULONG len=(ULONG)sizeof(ep)+5u+(ULONG)count*((ULONG)sizeof(fmt)+4u);
    ep.Length=len;ep.LinkType=link;ep.HwVendorId=0x25du;ep.HwDeviceId=0x722u;
    memcpy(p,&ep,sizeof(ep));memcpy(p+sizeof(ep),&cap,4);
    p[sizeof(ep)+4u]=count;
    fmt.Format.nSamplesPerSec=48000u;fmt.Format.nChannels=2u;
    fmt.Format.wBitsPerSample=24u;
    for(ULONG i=0;i<count;++i) {
        UCHAR *q=p+sizeof(ep)+5u+i*(sizeof(fmt)+4u);
        memcpy(q,&fmt,sizeof(fmt));memcpy(q+sizeof(fmt),&cap,4);
    }
    return len;
}
int main(void) {
    UCHAR bytes[1200]={0}; SOF_NHLT_INFO info;
    const ULONG first=(ULONG)sizeof(ACPI_NHLT_HEADER)+1u;
    ACPI_NHLT_HEADER *h=(ACPI_NHLT_HEADER*)bytes;
    ULONG len,cap=0xffffffffu;
    assert(sizeof(WAVEFORMATEXTENSIBLE)==40u);
    h->Signature=NHLT_ACPI_SIGNATURE;bytes[first-1u]=1u;
    len=first+make_ep(bytes+first,NHLT_LINK_DMIC,1u);
    fix_checksum(bytes,len);
    assert(SofNhltParse(bytes,len,&info)==STATUS_SUCCESS);
    assert(info.IsValid && info.Dmic.Present && info.Dmic.ChannelCount==2u);
    assert(info.Dmic.SampleRate==48000u && info.Dmic.BitDepth==24u);
    bytes[first-1u]=2u;fix_checksum(bytes,len);
    assert(SofNhltParse(bytes,len,&info)==STATUS_INVALID_PARAMETER);
    assert(!info.IsValid && info.TotalEndpoints==0u);
    bytes[first-1u]=1u;
    fix_checksum(bytes,len-1u);
    assert(SofNhltParse(bytes,len-1u,&info)==STATUS_INVALID_PARAMETER);
    assert(!info.IsValid);
    memcpy(bytes+first+sizeof(NHLT_ENDPOINT_DESCRIPTOR),&cap,4);
    fix_checksum(bytes,len);
    assert(SofNhltParse(bytes,len,&info)==STATUS_INVALID_PARAMETER);
    len=first+make_ep(bytes+first,NHLT_LINK_SDW,0u);
    fix_checksum(bytes,len);
    assert(SofNhltParse(bytes,len,&info)==STATUS_SUCCESS);
    assert(info.SdwEndpointsCount==1u && info.SdwEndpoints[0].SampleRate==0u);
    bytes[len-1u]^=1u;
    assert(SofNhltParse(bytes,len,&info)==STATUS_INVALID_PARAMETER);
    bytes[first-1u]=9u;len=first;
    for(unsigned i=0;i<9u;++i) len+=make_ep(bytes+len,NHLT_LINK_SDW,0u);
    fix_checksum(bytes,len);
    assert(SofNhltParse(bytes,len,&info)==STATUS_INVALID_PARAMETER);
    assert(info.SdwEndpointsCount==0u && !info.IsValid);
    puts("PASS: actual NHLT parser bounds, checksum, full endpoints, no fabricated format defaults");
    return 0;
}
'''
# Header layout is extracted rather than redefining endpoint/summary structures.
with tempfile.TemporaryDirectory(prefix="pj_nhlt_") as temp:
    src = Path(temp) / "nhlt.c"
    exe = Path(temp) / ("nhlt.exe" if os.name == "nt" else "nhlt")
    src.write_text(shim + consts + packed + "\n#pragma pack(pop)\n" + info + body + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(src), "-o", str(exe)], check=True, timeout=40)
    subprocess.run([str(exe)], check=True, timeout=15)

sdw = (root / "src/driver/SdwController.c").read_text().split("SofSdwEnumerateChildren(", 1)[1]
sdw = sdw.split("SofIsSdcaFunctionDevice(", 1)[0]
assert "applying ALC722 Single-Lane" not in sdw
assert "Ctx->NhltInfo.SdwEndpointsCount > SOF_MAX_SDW_ENDPOINTS" in sdw
assert "!ep->Present || vId == 0 || dId == 0" in sdw
print("PASS: absent or invalid SoundWire descriptors do not manufacture child devices")
