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
disconnect = source[source.index("NTSTATUS SofDisconnectAudioChild("):
                    source.index("#ifdef PUJJO_ENABLE_EXPERIMENTAL_AUDIO\nstatic NTSTATUS AudioSelectFormat(")]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned ULONG;
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
typedef struct { STREAM *DmicStream; int AudioOnline,AudioDmaFaulted,Position; } DEVICE, *PSOF_DEVICE_CONTEXT;
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
static void AudioSetPositionEnabled(DEVICE *d,int v) { assert(locked);d->Position=v; }
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
    puts("PASS: production DMIC capture start, rollback, faulted stop and detached lease cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dmic_route_") as tmp:
    src=Path(tmp)/"route.c"
    exe=Path(tmp)/("route.exe" if os.name=="nt" else "route")
    src.write_text(prefix+callbacks+disconnect+suffix)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=15)
