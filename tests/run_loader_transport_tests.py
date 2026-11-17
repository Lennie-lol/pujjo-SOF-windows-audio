#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute exact production function bodies using host-only Windows API doubles.
Not a WDK compile: API declarations are deliberately limited test substitutes.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
def function(path, name):
    source = (ROOT/path).read_text()
    start = re.search(r'(?m)^(?:static )?(?:int|NTSTATUS)\s*\n?' + name + r'\(', source)
    if not start:
        raise RuntimeError(name)
    end = source.index('\n}', start.start()) + 2
    return source[start.start():end]
loader = function('src/driver/Loader.c', 'SofParseFirmwareImage')
spb = function('src/driver/peripheral/PujjoRt5682sSpbWindows.c', 'Pj5682sSpbRead')
write = function('src/driver/peripheral/PujjoRt5682sSpbWindows.c', 'Pj5682sSpbWrite')
header = (ROOT/'include/sof/ext_manifest4.h').read_text()
structs = header[header.index('struct sof_ext_manifest4_hdr {'):header.index('#endif /* __SOF_FIRMWARE_EXT_MANIFEST4_H__ */')]
info = (ROOT/'src/driver/Loader.h').read_text()
info = info[info.index('typedef struct _SOF_FW_IMAGE_INFO {'):info.index('} SOF_FW_IMAGE_INFO, *PSOF_FW_IMAGE_INFO;')+len('} SOF_FW_IMAGE_INFO, *PSOF_FW_IMAGE_INFO;')]
shim = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {uint8_t b[16];} guid_t;
#define MAX_MODULE_NAME_LEN 8
#define DEFAULT_HASH_SHA256_LEN 32
typedef int NTSTATUS;
typedef void *PVOID;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef unsigned char UCHAR;
/* Source prototypes now use portable Pujjo types rather than CRT stdint
 * in WDK builds. Keep host doubles ABI-identical to the production callbacks. */
typedef uint8_t PJ_BYTE;
typedef size_t PJ_SIZE;
typedef char CHAR;
#define MAXULONG UINT32_MAX
#define SOF_MAN4_FW_HDR_OFFSET 0x2000
#define SOF_FW_MAX_NAME_LEN 16
#define MAX_FW_BINARY_NAME 8
#define __packed __attribute__((packed))
#define SOF_EXT_MAN4_MAGIC_NUMBER 0x31454124
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_INVALID_IMAGE_FORMAT -2
#define STATUS_INVALID_BUFFER_SIZE -3
#define _In_reads_bytes_(x)
#define _In_
#define _Out_
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory memcpy
#define min(a,b) ((a)<(b)?(a):(b))
#define DbgPrintEx(...) ((void)0)
#define NT_SUCCESS(s) ((s)>=0)
#define WDF_NO_HANDLE NULL
#define IOCTL_SPB_EXECUTE_SEQUENCE 1
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define PJ5682S_SPB_TIMEOUT_MS 1000
#define WDF_REL_TIMEOUT_IN_MS(x) (x)
typedef struct {int Opened; void *Target; void *Lock; int Powered;} PJ5682S_SPB_WINDOWS;
#define PAGED_CODE() ((void)0)
static int lock_depth;
static void WdfWaitLockAcquire(void *lock,void *timeout) {
    (void)timeout;assert(lock && !lock_depth);lock_depth=1;
}
static void WdfWaitLockRelease(void *lock) {assert(lock && lock_depth==1);lock_depth=0;}
typedef struct {int dummy;} WDF_MEMORY_DESCRIPTOR;
typedef struct {int Timeout;} WDF_REQUEST_SEND_OPTIONS;
#define SPB_TRANSFER_LIST_AND_ENTRIES(n) struct {struct {int Transfers[n];} List;}
#define SPB_TRANSFER_LIST_INIT(...) ((void)0)
#define SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(...) 0
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(p,buf,n) ((void)(buf),(void)(n),(p)->dummy=0)
#define WDF_REQUEST_SEND_OPTIONS_INIT(p,...) ((p)->Timeout=0)
static ULONG_PTR mock_bytes;
static int mock_status;
static unsigned calls;
static NTSTATUS WdfIoTargetSendIoctlSynchronously(void *target, void *request,
    int ioctl, WDF_MEMORY_DESCRIPTOR *input, void *output,
    WDF_REQUEST_SEND_OPTIONS *options, ULONG_PTR *bytes) {
    (void)target;(void)request;(void)ioctl;(void)input;(void)output;
    assert(options->Timeout==1000); ++calls;
    if(bytes) *bytes=mock_bytes;
    return mock_status;
}
static NTSTATUS WdfIoTargetSendWriteSynchronously(void *target, void *request,
    WDF_MEMORY_DESCRIPTOR *input, void *offset, WDF_REQUEST_SEND_OPTIONS *options,
    ULONG_PTR *bytes) {
    return WdfIoTargetSendIoctlSynchronously(target,request,0,input,offset,options,bytes);
}
'''
main = r'''
int main(int argc, char **argv) {
    uint8_t image[16384]={0}, tx[4]={0}, rx[2]={0};
    struct sof_ext_manifest4_hdr h={SOF_EXT_MAN4_MAGIC_NUMBER,16,1,0,0};
    SOF_FW_IMAGE_INFO info;
    PJ5682S_SPB_WINDOWS state={1,(void *)&info,(void *)&info,1};
    struct sof_man4_fw_binary_header fw={0};
    fw.id=UINT32_C(0x314d4124);fw.len=sizeof(fw);fw.major_version=7;fw.num_module_entries=3;
    memcpy(fw.name,"BASEFW",6);
    memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    for (unsigned i=0;i<3;++i) {
        struct sof_man4_module valid_module={0};valid_module.id=UINT32_C(0x454d4124);
        memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET+sizeof(fw)+i*sizeof(valid_module),&valid_module,sizeof(valid_module));
    }
    memcpy(image,&h,sizeof(h));
    assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_SUCCESS);
    assert(info.PayloadOffset==16 && info.PayloadSize==sizeof(image)-16);
    assert(info.MajorVersion==7 && info.NumModules==3 && !strcmp(info.BinaryName,"BASEFW"));
    fw.id=0;memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_INVALID_IMAGE_FORMAT);
    fw.id=UINT32_C(0x314d4124);
    fw.len=0;memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_INVALID_IMAGE_FORMAT);
    assert(info.RawData==NULL);
    fw.len=sizeof(fw);memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    assert(SofParseFirmwareImage(image,16+SOF_MAN4_FW_HDR_OFFSET+sizeof(fw)-1,&info)==STATUS_INVALID_BUFFER_SIZE);
    for (uint32_t n=0;n<16;++n) {
        h.len=n;memcpy(image,&h,sizeof(h));
        assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_INVALID_BUFFER_SIZE);
    }
    h.len=256;memcpy(image,&h,sizeof(h));
    assert(SofParseFirmwareImage(image,256,&info)==STATUS_INVALID_BUFFER_SIZE);
    h.len=UINT32_MAX;memcpy(image,&h,sizeof(h));
    assert(SofParseFirmwareImage(image,256,&info)==STATUS_INVALID_BUFFER_SIZE);
    h.len=16;h.id=0;memcpy(image,&h,sizeof(h));
    assert(SofParseFirmwareImage(image,256,&info)==STATUS_INVALID_IMAGE_FORMAT);
    assert(SofParseFirmwareImage(NULL,0,&info)==STATUS_INVALID_PARAMETER);
    assert(info.RawData==NULL && info.RawSize==0);
    assert(SofParseFirmwareImage(image,15,&info)==STATUS_INVALID_PARAMETER);
    assert(SofParseFirmwareImage(image,256,NULL)==STATUS_INVALID_PARAMETER);
#if SIZE_MAX > UINT32_MAX
    assert(SofParseFirmwareImage(image,(size_t)UINT32_MAX+1,&info)==STATUS_INVALID_BUFFER_SIZE);
#endif
    /* Deterministic malformed-manifest fuzz: actual allocated buffer bounds. */
    uint32_t seed=0x56821019;
    for (unsigned i=0;i<20000;++i) {
        seed=seed*1664525u+1013904223u;
        h.id=(i%3==0)?0:SOF_EXT_MAN4_MAGIC_NUMBER;
        h.len=(i%2==0)?seed:(seed%16384u);
        memcpy(image,&h,sizeof(h));
        size_t n=seed%sizeof(image);
        NTSTATUS status=SofParseFirmwareImage(image,n,&info);
        if (status==STATUS_SUCCESS) {
            assert(info.PayloadOffset>=sizeof(h));
            assert(info.PayloadOffset<=n);
            assert(info.PayloadSize==n-info.PayloadOffset);
        } else assert(info.RawData==NULL);
    }
    /* Module and config tables must fit; no giant multiplication wraps. */
    h.id=SOF_EXT_MAN4_MAGIC_NUMBER;h.len=16;memcpy(image,&h,sizeof(h));
    fw.num_module_entries=UINT32_MAX;
    memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_INVALID_IMAGE_FORMAT);
    fw.num_module_entries=3;memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET,&fw,sizeof(fw));
    struct sof_man4_module module={0};module.id=UINT32_C(0x454d4124);module.cfg_count=UINT16_MAX;module.cfg_offset=UINT16_MAX;
    memcpy(image+16+SOF_MAN4_FW_HDR_OFFSET+sizeof(fw),&module,sizeof(module));
    assert(SofParseFirmwareImage(image,sizeof(image),&info)==STATUS_INVALID_IMAGE_FORMAT);
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));
        long n=ftell(f);assert(n>0 && n<=16*1024*1024);rewind(f);
        void *bytes=malloc((size_t)n);assert(bytes);
        assert(fread(bytes,1,(size_t)n,f)==(size_t)n);fclose(f);
        assert(SofParseFirmwareImage(bytes,(size_t)n,&info)==STATUS_SUCCESS);
        printf("PASS: external firmware metadata %s %u.%u.%u.%u modules=%u payload=%u (NOT signature verification)\n",
          info.BinaryName,info.MajorVersion,info.MinorVersion,info.HotfixVersion,info.BuildVersion,info.NumModules,info.PayloadSize);
        free(bytes);
    }
    /* Every partial count, oversized completion, error and closed target. */
    for (mock_bytes=0;mock_bytes<=5;++mock_bytes) {
        assert((Pj5682sSpbRead(&state,tx,2,rx,2)==0)==(mock_bytes==4));
        assert((Pj5682sSpbWrite(&state,tx,4)==0)==(mock_bytes==4));
    }
    mock_bytes=4; mock_status=-1;
    assert(Pj5682sSpbRead(&state,tx,2,rx,2)==-1);
    assert(Pj5682sSpbWrite(&state,tx,4)==-1);
    mock_status=0;
    unsigned before=calls;
    assert(Pj5682sSpbRead(&state,tx,1,rx,2)==-1);
    assert(Pj5682sSpbRead(NULL,tx,2,rx,2)==-1);
    assert(Pj5682sSpbWrite(&state,tx,3)==-1);
    state.Opened=0;
    assert(Pj5682sSpbRead(&state,tx,2,rx,2)==-1);
    assert(Pj5682sSpbWrite(&state,tx,4)==-1);
    assert(calls==before && lock_depth==0);
    state.Opened=1;state.Powered=0;
    assert(Pj5682sSpbRead(&state,tx,2,rx,2)==-1);
    assert(Pj5682sSpbWrite(&state,tx,4)==-1);
    assert(calls==before && lock_depth==0);
    puts("PASS: production loader bounds and SPB short/error transfer regressions (host doubles)");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pujjo_loader_') as d:
    src=Path(d)/'regressions.c'; exe=Path(d)/('regressions.exe' if os.name=='nt' else 'regressions')
    src.write_text(shim+structs+info+loader+spb+write+main)
    cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
         *shlex.split(os.environ.get('CFLAGS','')),str(src),'-o',str(exe)]
    print(' '.join(cmd),flush=True)
    subprocess.run(cmd,check=True)
    subprocess.run([str(exe)]+([os.environ['PUJJO_FIRMWARE_TEST_FILE']] if os.environ.get('PUJJO_FIRMWARE_TEST_FILE') else []),check=True,timeout=30)
