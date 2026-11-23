"""Run the production IPC4 DPC with a latched cAVS doorbell and mock events."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[1] / "src/driver"
ipc = (driver / "Ipc.c").read_text()
start = ipc.index("BOOLEAN\nSofIpcDpcHandler(")
dpc = ipc[start:ipc.index("\n}", start) + 2]
shim = r"""
#include "PujjoIpc4Protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG, *PULONG;
typedef unsigned char UCHAR, *PUCHAR;
typedef int BOOLEAN, KIRQL;
#define TRUE 1
#define FALSE 0
#define _In_
#define MAXULONG UINT32_MAX
#define DPFLTR_DEFAULT_ID 0
#define DPFLTR_INFO_LEVEL 0
#define DPFLTR_ERROR_LEVEL 0
#define IO_NO_INCREMENT 0
#define SOF_IPC4_MSG_DIR_MASK PJ_IPC4_DIRECTION_MASK
#define SOF_IPC4_REPLY_STATUS 0xffffffu
#define SOF_IPC4_NOTIFICATION_TYPE_GET(p) (((p)>>16)&255u)
#define SOF_IPC4_LOG_CORE_GET(p) ((p)&15u)
#define SOF_IPC4_NOTIFY_FW_READY 8u
#define SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT 10u
#define SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS 6u
enum { SOF_FW_BOOT_NOT_STARTED, SOF_FW_BOOT_IN_PROGRESS, SOF_FW_BOOT_READY_RECEIVED,
       SOF_FW_BOOT_COMPLETE, SOF_FW_BOOT_FAILED };
typedef struct {
    unsigned HipcIda, HipcTdr, HipcTdd, HipcTda, HipcCtl, MailboxUplinkOffset;
    ULONG HipcIdaDone, HipcTdrBusy, HipcTdaDone;
    int IsAce;
} HW;
typedef struct {
    HW *HwDesc; ULONG *Bar4VirtualAddress; unsigned Bar4Length;
    int IpcLock, IpcTxBusy, IpcFaulted, IpcReplyReady, FwState, FwReadyEvent, IpcReplyEvent;
    ULONG IpcRequestPrimary, IpcRequestExtension, IpcReplyCapacity;
    struct { ULONG Primary, Extension, PayloadSize; void *Payload; } PendingReply;
    UCHAR *ReplyBuffer;
} CTX, *PSOF_DEVICE_CONTEXT;
static ULONG bar[16];
static HW hw={0,4,8,12,16,32,1u<<31,1u<<31,1u<<31,0};
static int ack_phase, flushed;
static int SofDeviceRemoved(CTX *c) { (void)c; return 0; }
static ULONG SofDspRead32(ULONG *base, unsigned offset) {
    if (offset==hw.HipcCtl) flushed=1;
    return base[offset/4];
}
static void SofDspWrite32(ULONG *base, unsigned offset, ULONG value) {
    assert(value==(1u<<31));
    if (offset==hw.HipcTdr) { assert(!ack_phase); ack_phase=1; base[offset/4]&=~value; }
    else if (offset==hw.HipcTda) { assert(ack_phase==1); ack_phase=2; }
    else { assert(offset==hw.HipcIda); base[offset/4]&=~value; }
}
#define SofDspUpdateBits(...) assert(0)
#define READ_REGISTER_ULONG(p) (*(p))
#define RtlStringCbPrintfA snprintf
static void DbgPrintEx(int id, int level, const char *fmt, ...) { (void)id;(void)level;(void)fmt; }
static void SofLogBreadcrumb(const char *s) { (void)s; }
static void KeAcquireSpinLock(int *lock,KIRQL *old) { assert(!*lock); *lock=1; *old=0; }
static void KeReleaseSpinLock(int *lock,KIRQL old) { assert(*lock && !old); *lock=0; }
static void KeSetEvent(int *event,int increment,int wait) {
    assert(!increment && !wait && ack_phase==2 && flushed); ++*event;
}
"""
cases = r"""
static int receive(CTX *c, ULONG primary, ULONG ext) {
    ack_phase=flushed=0;
    bar[hw.HipcTdr/4]=primary|hw.HipcTdrBusy;
    bar[hw.HipcTdd/4]=ext;
    return SofIpcDpcHandler(c);
}
int main(void) {
    UCHAR payload[16]={0};
    CTX initial={.HwDesc=&hw,.Bar4VirtualAddress=bar,.Bar4Length=sizeof(bar),
                 .FwState=SOF_FW_BOOT_IN_PROGRESS,.ReplyBuffer=payload};
    CTX c=initial;
    const ULONG ready=0x1b080000u, get=PJ_IPC4_MODULE_LARGE_GET;
    assert(receive(&c,ready,0) && c.FwReadyEvent==1 && c.FwState==SOF_FW_BOOT_READY_RECEIVED);
    c.IpcTxBusy=1; c.IpcRequestPrimary=get; c.IpcRequestExtension=(1u<<29)|(7u<<20)|16u;
    c.IpcReplyCapacity=sizeof(payload);
    bar[hw.MailboxUplinkOffset/4]=0x04030201;
    assert(receive(&c,get|PJ_IPC4_DIRECTION_MASK,(3u<<28)|(7u<<20)|4u));
    assert(c.IpcReplyEvent==1 && c.IpcReplyReady && !c.IpcFaulted);
    assert(c.PendingReply.PayloadSize==4 && payload[0]==1 && payload[3]==4 && !payload[4]);
    assert(receive(&c,ready,0) && c.IpcFaulted && c.FwState==SOF_FW_BOOT_FAILED);
    c=initial; assert(receive(&c,ready,1) && c.IpcFaulted);
    c=initial; c.FwState=SOF_FW_BOOT_NOT_STARTED;
    assert(receive(&c,ready,0) && c.FwState==SOF_FW_BOOT_FAILED);
    c=initial; assert(receive(&c,get|PJ_IPC4_DIRECTION_MASK,0) && c.IpcFaulted);
    c=initial; c.IpcTxBusy=1; c.IpcRequestPrimary=get;
    assert(receive(&c,0x21000000u,0) && c.IpcFaulted && c.IpcReplyEvent==1);
    c=initial; bar[hw.HipcCtl/4]=MAXULONG;
    assert(!receive(&c,ready,0) && !c.FwReadyEvent && c.FwState==SOF_FW_BOOT_IN_PROGRESS);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp) / "dpc.c"
    exe = Path(temp) / ("dpc.exe" if os.name == "nt" else "dpc")
    source.write_text(shim + dpc + cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), "-I", str(driver),
                    str(source), str(driver / "PujjoIpc4Protocol.c"), "-o", str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)
print("PASS: cold FW_READY and reply DPC event ordering, orphan and duplicate faults (host MMIO)")
