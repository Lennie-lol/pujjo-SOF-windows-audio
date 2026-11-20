#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise production DMIC callbacks against fault-injected host fixtures."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / "src/driver/PujjoAudioBus.c").read_text()
callbacks = source[source.index("static NTSTATUS AudioStopCapture("):
                   source.index("static NTSTATUS AudioPosition(")]
position = source[source.index("static NTSTATUS AudioPosition("):
                  source.index("static NTSTATUS AudioNotification(")]
disconnect = source[source.index("NTSTATUS SofDisconnectAudioChild("):
                    source.index("#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO\nstatic NTSTATUS AudioSelectFormat(")]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned ULONG, *PULONG;
typedef int KIRQL;
typedef int NTSTATUS, BOOLEAN;
typedef void *PVOID;
#define VOID void
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_INVALID_PARAMETER -2
#define STATUS_TEST_FAILURE -3
#define NT_SUCCESS(s) ((s)>=0)
typedef enum { KSSTATE_STOP, KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN } KSSTATE;
typedef struct { KSSTATE State; int Detached,DescriptorPublished; } STREAM, *PSOF_WAVERT_STREAM;
typedef struct { STREAM *DmicStream; int AudioOnline,AudioDmaFaulted,Position,AudioPositionEnabled,AudioPositionLock; } DEVICE, *PSOF_DEVICE_CONTEXT;
enum { MUTE=1,UNMUTE,RUN,STOP,PAUSE,DETACH,FREE,ACQUIRE };
static int trace[32],n,fail1,fail2,locked;
static int event(int op) {
    assert(locked && n<32);
    trace[n++]=op;
    return op==fail1||op==fail2?STATUS_TEST_FAILURE:0;
}
static void AudioLock(DEVICE *d) { assert(d&&!locked);locked=1; }
static void AudioUnlock(DEVICE *d) { assert(d&&locked);locked=0; }
static void AudioSetOnline(DEVICE *d,int v) { assert(locked);d->AudioOnline=v; }
static void AudioSetPositionEnabled(DEVICE *d,int v) { assert(locked);d->Position=d->AudioPositionEnabled=v; }
static void KeAcquireSpinLock(int *lock,KIRQL *irql) { assert(!*lock);*lock=1;*irql=0; }
static void KeReleaseSpinLock(int *lock,KIRQL irql) { assert(*lock&&irql==0);*lock=0; }
static int SofWaveRtGetPosition(STREAM *s,PULONG p) { assert(s&&p);*p=128;return 0; }
static int SofWaveRtSetMute(STREAM *s,int mute) { assert(s&&!s->Detached);return event(mute?MUTE:UNMUTE); }
static int SofWaveRtSetState(STREAM *s,KSSTATE state) {
    assert(s&&!s->Detached);
    int rc=event(state==KSSTATE_RUN?RUN:state==KSSTATE_PAUSE?PAUSE:state==KSSTATE_ACQUIRE?ACQUIRE:STOP);
    if(!rc) { s->State=state;s->DescriptorPublished=state==KSSTATE_RUN||state==KSSTATE_PAUSE; }
    return rc;
}
static int SofWaveRtDetachStream(STREAM *s) {
    assert(s);int rc=event(DETACH);s->Detached=1;return rc;
}
static int SofWaveRtFreeStream(STREAM *s) { assert(s);return event(FREE); }
'''
suffix = r'''
static DEVICE d;
static STREAM s,other;
static void reset(void) {
    memset(&d,0,sizeof(d));memset(&s,0,sizeof(s));n=fail1=fail2=locked=0;
    d.DmicStream=&s;d.AudioOnline=1;
}
static void expect(const int *ops,int count) {
    assert(n==count&&!locked);assert(!memcmp(trace,ops,(size_t)count*sizeof(int)));
}
int main(void) {
    reset();assert(AudioSetState(&d,&s,KSSTATE_RUN)==0);
    expect((int[]){MUTE,RUN,UNMUTE},3);assert(d.Position);
    n=0;assert(AudioSetState(&d,&s,KSSTATE_RUN)==0);
    expect((int[]){RUN,UNMUTE},2);
    n=0;assert(AudioSetState(&d,&s,KSSTATE_PAUSE)==0);
    expect((int[]){MUTE,PAUSE},2);
    n=0;assert(AudioSetState(&d,&s,KSSTATE_RUN)==0);expect((int[]){MUTE,RUN,UNMUTE},3);
    n=0;d.AudioDmaFaulted=1;assert(AudioSetState(&d,&s,KSSTATE_RUN)==STATUS_DEVICE_NOT_READY&&!n);
    assert(AudioSetState(&d,&s,KSSTATE_STOP)==0);expect((int[]){MUTE,STOP},2);assert(!d.Position);
    reset();fail1=UNMUTE;assert(AudioSetState(&d,&s,KSSTATE_RUN)==STATUS_TEST_FAILURE);
    expect((int[]){MUTE,RUN,UNMUTE,MUTE,STOP},5);assert(!d.Position&&!d.AudioDmaFaulted);
    reset();fail1=UNMUTE;fail2=STOP;assert(AudioSetState(&d,&s,KSSTATE_RUN)==STATUS_TEST_FAILURE);
    assert(d.AudioDmaFaulted&&!d.Position);
    reset();s.State=KSSTATE_RUN;fail1=MUTE;
    assert(AudioSetState(&d,&s,KSSTATE_STOP)==STATUS_TEST_FAILURE);expect((int[]){MUTE,STOP},2);
    assert(d.AudioDmaFaulted&&!d.Position);
    reset();fail1=RUN;assert(AudioSetState(&d,&s,KSSTATE_RUN)==STATUS_TEST_FAILURE);
    expect((int[]){MUTE,RUN,STOP},3);assert(!d.Position);
    reset();s.State=KSSTATE_RUN;fail1=MUTE;
    assert(SofDisconnectAudioChild(&d)==STATUS_TEST_FAILURE);expect((int[]){MUTE,DETACH},2);
    assert(!d.AudioOnline&&!d.Position&&d.AudioDmaFaulted&&s.Detached);
    n=0;AudioFree(&d,&s);expect((int[]){FREE},1);assert(!d.DmicStream);
    reset();s.State=KSSTATE_RUN;AudioFree(&d,&s);expect((int[]){MUTE,STOP,FREE},3);assert(!d.DmicStream&&!d.Position);
    reset();assert(AudioSetState(&d,&other,KSSTATE_RUN)==STATUS_DEVICE_NOT_READY&&!n);
    assert(AudioSetState(&d,&s,99)==STATUS_INVALID_PARAMETER&&!n);
    reset();assert(AudioSetState(&d,&s,KSSTATE_RUN)==0);
    ULONG pos=99;
    assert(AudioPosition(&d,&s,&pos)==0&&pos==128);
    pos=99;assert(AudioPosition(&d,&other,&pos)==STATUS_DEVICE_NOT_READY&&pos==99);
    assert(AudioPosition(&d,NULL,&pos)==STATUS_INVALID_PARAMETER);
    assert(AudioPosition(&d,&s,NULL)==STATUS_INVALID_PARAMETER);
    AudioFree(&d,&s);
    assert(AudioPosition(&d,&s,&pos)==STATUS_DEVICE_NOT_READY&&pos==99&&!d.AudioPositionLock);
    puts("PASS: production DMIC capture lifecycle and position ownership");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dmic_route_") as tmp:
    src=Path(tmp)/"route.c"
    exe=Path(tmp)/("route.exe" if os.name=="nt" else "route")
    src.write_text(prefix+callbacks+position+disconnect+suffix)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=15)

# Compile the actual capture format selector with mock NHLT and firmware
# catalog lookups. Publication must never advertise an unusable gateway.
selector = source[source.index("static NTSTATUS AudioSelectFormat("):
                  source.index("\n#endif", source.index("static NTSTATUS AudioSelectFormat("))]
select_prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_NOT_FOUND -2
#define STATUS_BUFFER_TOO_SMALL -3
#define STATUS_INVALID_BUFFER_SIZE -4
#define STATUS_OBJECT_NAME_COLLISION -5
#define NT_SUCCESS(x) ((x)>=0)
#define NHLT_LINK_DMIC 2
#define NHLT_DIRECTION_CAPTURE 1
#define NHLT_DEVICE_DMIC 0
#define PJ_CATALOG_OK 0
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
enum { PujjoAudioDmic=0 };
struct sof_ipc4_gtw_attributes { ULONG flags; };
struct sof_ipc4_copier_data { ULONG words[16]; };
typedef struct { ULONG LinkType,VirtualBusId,Direction,DeviceType,SampleRate,Channels,ContainerBits,ValidBits; } SOF_NHLT_CONFIG_KEY;
typedef struct { ULONG Rate,Channels,ContainerBits,ValidBits,Role; } FORMAT;
typedef struct { const void *NhltTable; ULONG NhltTableSize; int CopierProfile,GainProfile; FORMAT AudioFormat; } DEVICE,*PSOF_DEVICE_CONTEXT;
typedef struct { ULONG unused; } pj_firmware_config;
static int calls, allowed, first_status, reject_chunk, too_many_pages;
static ULONG blob_bytes;
static int SofNhltCopyConfiguration(const void *table,ULONG size,const SOF_NHLT_CONFIG_KEY *key,void *dest,ULONG cap,ULONG *bytes) {
    assert(table && size && !dest && !cap);
    assert(key->LinkType==2 && key->VirtualBusId==0 && key->Direction==1 && key->DeviceType==0);
    assert(key->SampleRate==48000 && key->Channels==2);
    ++calls;
    if(first_status) return first_status;
    if(calls!=allowed && allowed!=4) return STATUS_NOT_FOUND;
    *bytes=blob_bytes;return STATUS_BUFFER_TOO_SMALL;
}
static int pj_firmware_plan_ll(int *p,ULONG ibs,ULONG obs,pj_firmware_config *c,ULONG *pages) {
    assert(p && c && ibs==obs && (ibs==192 || ibs==384));
    *pages=too_many_pages?10:2;
    return reject_chunk==(int)ibs?1:0;
}
'''
select_cases = r'''
int main(void) {
    int table=1;
    DEVICE d={&table,1,0,0,{1,2,3,4,99}};
    assert(AudioSelectFormat(NULL)==STATUS_INVALID_PARAMETER);
    d.NhltTableSize=0;assert(AudioSelectFormat(&d)==STATUS_NOT_FOUND && !calls);d.NhltTableSize=1;
    blob_bytes=96;
    allowed=0;assert(AudioSelectFormat(&d)==STATUS_NOT_FOUND && calls==3 && d.AudioFormat.Role==99);
    for(int format=1;format<=3;++format) {
        calls=0;allowed=format;
        assert(AudioSelectFormat(&d)==0 && calls==format);
        assert(d.AudioFormat.Rate==48000 && d.AudioFormat.Channels==2 && d.AudioFormat.Role==0);
        assert(d.AudioFormat.ContainerBits==(format==1?16u:32u));
        assert(d.AudioFormat.ValidBits==(format==1?16u:format==2?24u:32u));
    }
    calls=0;allowed=4;reject_chunk=192;
    assert(AudioSelectFormat(&d)==0 && calls==2 && d.AudioFormat.ValidBits==24);
    reject_chunk=0;calls=0;too_many_pages=1;
    assert(AudioSelectFormat(&d)==STATUS_NOT_FOUND && calls==3);
    too_many_pages=0;calls=0;first_status=STATUS_OBJECT_NAME_COLLISION;
    assert(AudioSelectFormat(&d)==STATUS_OBJECT_NAME_COLLISION && calls==1);first_status=0;
    for(ULONG bad=0;bad<=5;++bad) {
        calls=0;blob_bytes=bad;
        assert(AudioSelectFormat(&d)==STATUS_INVALID_BUFFER_SIZE && calls==1);
    }
    calls=0;blob_bytes=4096;
    assert(AudioSelectFormat(&d)==STATUS_INVALID_BUFFER_SIZE && calls==1);
    puts("PASS: actual DMIC format selection, rejected malformed gateways and firmware budgets");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dmic_format_") as tmp:
    src=Path(tmp)/"format.c"
    exe=Path(tmp)/("format.exe" if os.name=="nt" else "format")
    src.write_text(select_prefix+selector+select_cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=15)
