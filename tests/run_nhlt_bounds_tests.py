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
args_parser.add_argument("--complete", action="store_true",
                         help="Test the combined four-route candidate")
args = args_parser.parse_args()
if args.complete and args.candidate_dir is None:
    args_parser.error("--complete requires --candidate-dir")
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
if args.complete:
    from assemble_pujjo_complete_nhlt import audit, assemble_dmic_only
    complete = (folder / "complete-candidate-only.nhlt").read_bytes()
    dmic = (folder / "dmic/dmic0-reference.bin").read_bytes()
    audit(complete, ssp_table, dmic)
    dmic_only = (folder / "dmic/dmic-only-candidate.nhlt").read_bytes()
    if dmic_only != assemble_dmic_only(ssp_table, dmic):
        raise ValueError("DMIC-only review candidate differs from verified independent sources")
    fixture += "#define TEST_COMPLETE_NHLT 1\n"
    fixture += "\n".join("static const UCHAR %s[] = {%s};" % (name, ",".join(map(str,data)))
                         for name,data in (("complete_table",complete),("dmic_expected",dmic),("dmic_only_table",dmic_only))) + "\n"
analog_source = (root / "src/driver/PujjoAnalog.c").read_text()
analog_helpers = analog_source[analog_source.index("static ULONG SspWord"):
                               analog_source.index("/* Firmware may acknowledge RUN")]
analog_types = r'''
#define FALSE 0
#define STATUS_NOT_SUPPORTED -7
#define STATUS_DEVICE_NOT_READY -8
#define STATUS_INVALID_BUFFER_SIZE -9
#define PUJJO_REVIEWED_SSP_BYTES 96u
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
enum { PujjoAudioHeadphone=1, PujjoAudioHeadsetMic=2, PujjoAudioSpeaker=3 };
struct sof_ipc4_copier_data { ULONG words[16]; };
typedef struct {
    int Role;
    ULONG VerifiedSsp;
    UCHAR VerifiedSspConfig[96];
    struct { ULONG Rate,Channels,ContainerBits,ValidBits; } Format;
} PUJJO_ANALOG_PROFILE;
typedef struct { const void *NhltTable; ULONG NhltTableSize; } DEVICE, *PSOF_DEVICE_CONTEXT;
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
    pj_dmic_profile absent;
    assert(SofNhltInspectPujjoDmic(ssp_table,sizeof(ssp_table),&absent)==STATUS_NOT_FOUND);
    assert(!absent.bytes);
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
    DEVICE d={ssp_table,sizeof(ssp_table)};
    PUJJO_ANALOG_PROFILE p={0};
    p.Format.Rate=48000; p.Format.Channels=2;
    p.Format.ContainerBits=32; p.Format.ValidBits=24;
    for(int role=PujjoAudioHeadphone; role<=PujjoAudioSpeaker; ++role) {
        p.Role=role; p.VerifiedSsp=(role==PujjoAudioSpeaker);
        memcpy(p.VerifiedSspConfig,p.VerifiedSsp?ssp1_expected:ssp0_expected,96);
        assert(VerifySspConfiguration(&d,&p)==STATUS_SUCCESS);
        p.VerifiedSspConfig[0]^=1;
        assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
        p.VerifiedSspConfig[0]^=1;
        p.VerifiedSsp^=1;
        assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
        p.VerifiedSsp^=1;
        p.Format.ValidBits=32;
        assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
        p.Format.ValidBits=24;
    }
    assert(VerifySspConfiguration(NULL,&p)==STATUS_INVALID_PARAMETER);
    assert(VerifySspConfiguration(&d,NULL)==STATUS_INVALID_PARAMETER);
    UCHAR damaged[sizeof(ssp_table)];
    memcpy(damaged,ssp_table,sizeof(damaged));
    damaged[sizeof(damaged)-1]^=1;
    d.NhltTable=damaged;
    assert(!NT_SUCCESS(VerifySspConfiguration(&d,&p)));
    puts("PASS: candidate NHLT through production parser and analog profile verifier, all three routes");
}
#ifdef TEST_COMPLETE_NHLT
static void test_complete(void) {
    SOF_NHLT_INFO info;
    ULONG length=0;
    UCHAR payload[sizeof(dmic_expected)], damaged[sizeof(complete_table)];
    SOF_NHLT_CONFIG_KEY key={NHLT_LINK_DMIC,0,NHLT_DIRECTION_CAPTURE,NHLT_DEVICE_DMIC,48000,4,32,32};
    assert(SofNhltParse(complete_table,sizeof(complete_table),&info)==STATUS_SUCCESS);
    pj_dmic_profile profile;
    assert(SofNhltInspectPujjoDmic(complete_table,sizeof(complete_table),&profile)==STATUS_SUCCESS);
    assert(profile.bytes==sizeof(dmic_expected) && profile.pdm_clock_hz>=2400000 &&
           profile.pdm_clock_hz<=4800000);
    assert(SofNhltInspectPujjoDmic(ssp_table,sizeof(ssp_table),&profile)==STATUS_NOT_FOUND);
    assert(!profile.bytes);
    assert(SofNhltParse(dmic_only_table,sizeof(dmic_only_table),&info)==STATUS_SUCCESS);
    assert(info.TotalEndpoints==1 && info.DmicEndpointsCount==1 && !info.SspEndpointsCount && !info.SdwEndpointsCount);
    assert(SofNhltInspectPujjoDmic(dmic_only_table,sizeof(dmic_only_table),&profile)==STATUS_SUCCESS);
    assert(profile.bytes==sizeof(dmic_expected));
    /* Repaired ACPI checksum must not hide a corrupt routing word. */
    memcpy(damaged,dmic_only_table,sizeof(dmic_only_table));
    damaged[sizeof(dmic_only_table)-sizeof(dmic_expected)+28]^=1u;
    fix_checksum(damaged,sizeof(dmic_only_table));
    assert(SofNhltInspectPujjoDmic(damaged,sizeof(dmic_only_table),&profile)==STATUS_INVALID_PARAMETER);
    assert(!profile.bytes);
    assert(SofNhltParse(complete_table,sizeof(complete_table),&info)==STATUS_SUCCESS);
    assert(info.TotalEndpoints==4u && info.DmicEndpointsCount==1u && info.SspEndpointsCount==3u);
    assert(info.Dmic.Present && info.Dmic.ChannelCount==4u && info.Dmic.BitDepth==32u);
    assert(info.Dmic.SampleRate==48000u && info.ExternalCodecPresent && !info.SdwEndpointsCount);
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,NULL,0,&length)==STATUS_BUFFER_TOO_SMALL);
    assert(length==sizeof(dmic_expected));
    memset(payload,0xa5,sizeof(payload));
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload)-1,&length)==STATUS_BUFFER_TOO_SMALL);
    assert(payload[0]==0xa5);
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_SUCCESS);
    assert(length==sizeof(dmic_expected) && !memcmp(payload,dmic_expected,length));
    key.Channels=2;
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_NOT_FOUND);
    key.Channels=4;key.ValidBits=24;
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_NOT_FOUND);
    key.ValidBits=32;key.VirtualBusId=1;
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_NOT_FOUND);
    key.VirtualBusId=0;key.Direction=NHLT_DIRECTION_RENDER;
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_NOT_FOUND);
    key=(SOF_NHLT_CONFIG_KEY){NHLT_LINK_SSP,0,NHLT_DIRECTION_RENDER,NHLT_DEVICE_I2S,48000,2,32,24};
    for(unsigned route=0;route<3;++route) {
        key.VirtualBusId=route==2;key.Direction=route==1?NHLT_DIRECTION_CAPTURE:NHLT_DIRECTION_RENDER;
        assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_SUCCESS);
        assert(length==96u && !memcmp(payload,route==2?ssp1_expected:ssp0_expected,96));
    }
    key.Direction=NHLT_DIRECTION_CAPTURE;
    assert(SofNhltCopyConfiguration(complete_table,sizeof(complete_table),&key,payload,sizeof(payload),&length)==STATUS_NOT_FOUND);
    memcpy(damaged,complete_table,sizeof(damaged));damaged[sizeof(damaged)-1]^=1;
    assert(SofNhltParse(damaged,sizeof(damaged),&info)==STATUS_INVALID_PARAMETER);
    memcpy(damaged,complete_table,sizeof(damaged));fix_checksum(damaged,sizeof(damaged)-4u);
    assert(SofNhltParse(damaged,sizeof(damaged)-4u,&info)==STATUS_INVALID_PARAMETER);
    puts("PASS: production parser resolves all four exact candidate routes and rejects incorrect formats");
}
#endif
int main(void) {
    test_lookup();
#ifdef TEST_COMPLETE_NHLT
    test_complete();
#endif
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
    src.write_text('#include "PujjoDmicProfile.h"\n' + shim + consts + packed + "\n#pragma pack(pop)\n" + info + body + fixture + analog_types + analog_helpers + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), "-I"+str(root/"src/driver"), str(src),
                    str(root/"src/driver/PujjoDmicProfile.c"), "-o", str(exe)], check=True, timeout=40)
    subprocess.run([str(exe)], check=True, timeout=15)
if args.candidate_dir is not None:
    print("PASS: actual upstream-cross-checked SSP candidate artifacts through production NHLT parser")

sdw = (root / "src/driver/SdwController.c").read_text().split("SofSdwEnumerateChildren(", 1)[1]
sdw = sdw.split("SofIsSdcaFunctionDevice(", 1)[0]
assert "applying ALC722 Single-Lane" not in sdw
assert "Ctx->NhltInfo.SdwEndpointsCount > SOF_MAX_SDW_ENDPOINTS" in sdw
assert "!ep->Present || vId == 0 || dId == 0" in sdw
print("PASS: absent or invalid SoundWire descriptors do not manufacture child devices")
