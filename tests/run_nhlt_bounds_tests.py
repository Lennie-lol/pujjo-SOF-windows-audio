#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host execution of the actual NHLT parser with packed ABI-shaped test types."""
from pathlib import Path
import argparse
import os
import subprocess
import shlex
import tempfile
from test_prepare_ssp_candidate import blob, table, build
from generate_pujjo_ssp_candidate import candidate

root = Path(__file__).resolve().parents[1]
header = (root / "src/driver/Nhlt.h").read_text()
source = (root / "src/driver/Nhlt.c").read_text()
consts = header[header.index("#define NHLT_ACPI_SIGNATURE"):header.index("#include <pshpack1.h>")]
packed = header[header.index("typedef struct _ACPI_NHLT_HEADER"):header.index("#include <poppack.h>")]
info = header[header.index("typedef struct _SOF_NHLT_DMIC_INFO"):header.index("/* NHLT Parser Functions */")]
body = source[source.index("#define SOF_NHLT_MAX_TABLE_BYTES"):source.index("\nNTSTATUS SofNhltQueryAcpiTable(")]
shim = r'''
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define _KERNEL_MODE
typedef void *PVOID;
#define STATUS_OBJECT_NAME_NOT_FOUND -5
#define STATUS_INSUFFICIENT_RESOURCES -6
#define ExAllocatePool2(flags,size,tag) malloc(size)
#include <assert.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
typedef uint8_t BOOLEAN;
typedef int NTSTATUS;
typedef ULONG *PULONG;
#define TRUE 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_BUFFER_TOO_SMALL -2
#define STATUS_NOT_FOUND -3
#define STATUS_OBJECT_NAME_COLLISION -4
#define NT_SUCCESS(s) ((s) >= 0)
#define _In_reads_bytes_(n)
#define _In_
#define _Out_
#define _In_opt_
#define _In_reads_bytes_opt_(n)
#define _Out_writes_bytes_opt_(n)
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlEqualMemory(a,b,n) (memcmp((a),(b),(n)) == 0)
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))
#pragma pack(push,1)
typedef struct {
    struct {USHORT wFormatTag,nChannels; ULONG nSamplesPerSec,nAvgBytesPerSec;
            USHORT nBlockAlign,wBitsPerSample,cbSize;} Format;
    struct { USHORT wValidBitsPerSample; } Samples;
    ULONG channelMask;
    UCHAR SubFormat[16];
} WAVEFORMATEXTENSIBLE;
'''
const_names = ("ssp_table", "ssp0_expected", "ssp1_expected")
args_parser = argparse.ArgumentParser(description=__doc__)
args_parser.add_argument("--candidate-dir", type=Path,
                         help="Test the actual review-only ALSA comparison artifacts")
args = args_parser.parse_args()
if args.candidate_dir is None:
    ssp0, ssp1 = blob(0), blob(1)
    ssp_table, _ = build(table(0, ssp0), table(1, ssp1), ssp0, ssp1)
else:
    folder = args.candidate_dir
    ssp0 = (folder / "ssp0-candidate.bin").read_bytes()
    ssp1 = (folder / "ssp1-candidate.bin").read_bytes()
    ssp_table = (folder / "candidate-only.nhlt").read_bytes()
    if (ssp0, ssp1) != (candidate(0), candidate(1)):
        raise ValueError("candidate artifacts differ from fixed generator")
    expected_table, _ = build(
        (folder / "ssp0-reference.nhlt").read_bytes(),
        (folder / "ssp1-reference.nhlt").read_bytes(), ssp0, ssp1)
    if ssp_table != expected_table:
        raise ValueError("candidate NHLT differs from verified encoder comparison")
fixture = "\n".join("static const UCHAR %s[] = {%s};" % (name, ",".join(map(str, data)))
                    for name, data in zip(const_names, (ssp_table, ssp0, ssp1))) + "\n"
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
    ep.Length=len;ep.LinkType=link;ep.Direction=NHLT_DIRECTION_CAPTURE;
    ep.DeviceType=NHLT_DEVICE_DMIC;ep.VirtualBusId=1u;ep.HwVendorId=0x25du;ep.HwDeviceId=0x722u;
    memcpy(p,&ep,sizeof(ep));memcpy(p+sizeof(ep),&cap,4);
    p[sizeof(ep)+4u]=count;
    fmt.Format.nSamplesPerSec=48000u;fmt.Format.nChannels=2u;
    fmt.Format.wBitsPerSample=24u;
    fmt.Format.nBlockAlign=6u;
    fmt.Format.nAvgBytesPerSec=288000u;
    fmt.Format.wFormatTag=0xfffeu;
    fmt.Format.cbSize=22u;
    fmt.Samples.wValidBitsPerSample=20u;
    const UCHAR pcm[]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    memcpy(fmt.SubFormat,pcm,sizeof(pcm));
    for(ULONG i=0;i<count;++i) {
        UCHAR *q=p+sizeof(ep)+5u+i*(sizeof(fmt)+4u);
        memcpy(q,&fmt,sizeof(fmt));memcpy(q+sizeof(fmt),&cap,4);
    }
    return len;
}
static void visit(void *context, const NHLT_ENDPOINT_DESCRIPTOR *ep,
                  const UCHAR *ep_cfg, ULONG ep_bytes,
                  const WAVEFORMATEXTENSIBLE *format, const UCHAR *blob, ULONG blob_bytes) {
    unsigned *counts=context;
    assert(ep->VirtualBusId==1u && ep->DeviceType==NHLT_DEVICE_DMIC);
    assert(ep_cfg && ep_bytes==0u);
    if (format) {
        assert(format->Samples.wValidBitsPerSample==20u && blob && blob_bytes==4u);
        ++counts[1];
    } else {
        assert(!blob && !blob_bytes);
        ++counts[0];
    }
}
static void test_lookup(void) {
    UCHAR bytes[512]={0}, payload[]={0,0x12,0x34,0x56}, copied[4]={0};
    SOF_NHLT_CONFIG_KEY key={NHLT_LINK_DMIC,1,NHLT_DIRECTION_CAPTURE,NHLT_DEVICE_DMIC,48000,2,24,20};
    ACPI_NHLT_HEADER *h=(ACPI_NHLT_HEADER*)bytes;
    const ULONG first=(ULONG)sizeof(*h)+1u;
    ULONG bytes_needed=0, blob_bytes=sizeof(payload), ep_bytes;
    unsigned counts[2]={0};
    h->Signature=NHLT_ACPI_SIGNATURE;
    bytes[first-1u]=1u;
    ep_bytes=make_ep(bytes+first,NHLT_LINK_DMIC,1u);
    memcpy(bytes+first+ep_bytes-4u,&blob_bytes,4);
    memcpy(bytes+first+ep_bytes,payload,sizeof(payload));
    ep_bytes+=blob_bytes;
    memcpy(bytes+first,&ep_bytes,4);
    fix_checksum(bytes,first+ep_bytes);
    void *owned = NULL;
    ULONG owned_bytes = 0;
    assert(SofNhltQueryPujjoFallback(STATUS_NOT_FOUND,&owned,&owned_bytes)==STATUS_NOT_FOUND);
    assert(!owned && !owned_bytes);
    assert(SofNhltCopyFallback(STATUS_INVALID_PARAMETER,bytes,sizeof(bytes),&owned,&owned_bytes)==STATUS_INVALID_PARAMETER);
    assert(SofNhltCopyFallback(STATUS_NOT_FOUND,bytes,sizeof(bytes),&owned,&owned_bytes)==STATUS_SUCCESS);
    assert(owned != bytes && owned_bytes == sizeof(bytes));
    UCHAR from_fallback[4];
    assert(SofNhltCopyConfiguration(owned,owned_bytes,&key,from_fallback,4,&bytes_needed)==STATUS_SUCCESS);
    assert(!memcmp(payload,from_fallback,4));
    free(owned);
    assert(SofNhltEnumerate(bytes,sizeof(bytes),visit,counts)==STATUS_SUCCESS);
    assert(counts[0]==1u && counts[1]==1u);
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,NULL,0,&bytes_needed)==STATUS_BUFFER_TOO_SMALL);
    assert(bytes_needed==4u);
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,3,&bytes_needed)==STATUS_BUFFER_TOO_SMALL);
    assert(copied[1]==0);
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_SUCCESS);
    assert(memcmp(copied,payload,sizeof(payload))==0);

    {
        const ULONG avg_offset = first + sizeof(NHLT_ENDPOINT_DESCRIPTOR) + 5u + 8u;
        bytes[avg_offset] ^= 1u;
        fix_checksum(bytes,first+ep_bytes);
        assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_NOT_FOUND);
        assert(bytes_needed==0u && memcmp(copied,payload,sizeof(payload))==0);
        bytes[avg_offset] ^= 1u;
        fix_checksum(bytes,first+ep_bytes);
    }

    key.ValidBits=24u;
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_NOT_FOUND);
    assert(bytes_needed==0u);
    key.ValidBits=20u;
    key.VirtualBusId=0u;
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_NOT_FOUND);
    key.VirtualBusId=1u;

    bytes[first-1u]=2u;
    memcpy(bytes+first+ep_bytes,bytes+first,ep_bytes);
    fix_checksum(bytes,first+2u*ep_bytes);
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_SUCCESS);
    bytes[first+2u*ep_bytes-1u]^=1u;
    fix_checksum(bytes,first+2u*ep_bytes);
    assert(SofNhltCopyConfiguration(bytes,sizeof(bytes),&key,copied,4,&bytes_needed)==STATUS_OBJECT_NAME_COLLISION);
    assert(bytes_needed==0u && memcmp(copied,payload,sizeof(payload))==0);
    fix_checksum(bytes,first+2u*ep_bytes-1u);
    assert(SofNhltEnumerate(bytes,sizeof(bytes),visit,counts)==STATUS_INVALID_PARAMETER);
    assert(counts[0]==1u && counts[1]==1u);
}

static void test_generated_ssp(void) {
    SOF_NHLT_INFO info;
    SOF_NHLT_CONFIG_KEY key={NHLT_LINK_SSP,0,NHLT_DIRECTION_RENDER,NHLT_DEVICE_I2S,48000,2,32,24};
    UCHAR actual[96];
    ULONG length=0;
    assert(SofNhltParse(ssp_table,sizeof(ssp_table),&info)==STATUS_SUCCESS);
    assert(info.TotalEndpoints==3u && info.ExternalCodecPresent);
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_SUCCESS);
    assert(length==96 && !memcmp(actual,ssp0_expected,96));
    key.Direction=NHLT_DIRECTION_CAPTURE;
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_SUCCESS);
    assert(length==96 && !memcmp(actual,ssp0_expected,96));
    key.VirtualBusId=1;
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_NOT_FOUND);
    key.Direction=NHLT_DIRECTION_RENDER;
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_SUCCESS);
    assert(length==96 && !memcmp(actual,ssp1_expected,96));
    key.ValidBits=32;
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_NOT_FOUND);
    key.ValidBits=24;
    key.ContainerBits=24;
    assert(SofNhltCopyConfiguration(ssp_table,sizeof(ssp_table),&key,actual,96,&length)==STATUS_NOT_FOUND);
    puts("PASS: candidate generator to production NHLT parser, exact SSP PCM selection");
}
int main(void) {
    test_lookup();
    test_generated_ssp();
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
    src.write_text(shim + consts + packed + "\n#pragma pack(pop)\n" + info + body + fixture + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)], check=True, timeout=40)
    subprocess.run([str(exe)], check=True, timeout=15)
if args.candidate_dir is not None:
    print("PASS: actual upstream-cross-checked SSP candidate artifacts through production NHLT parser")

sdw = (root / "src/driver/SdwController.c").read_text().split("SofSdwEnumerateChildren(", 1)[1]
sdw = sdw.split("SofIsSdcaFunctionDevice(", 1)[0]
assert "applying ALC722 Single-Lane" not in sdw
assert "Ctx->NhltInfo.SdwEndpointsCount > SOF_MAX_SDW_ENDPOINTS" in sdw
assert "!ep->Present || vId == 0 || dId == 0" in sdw
print("PASS: absent or invalid SoundWire descriptors do not manufacture child devices")
