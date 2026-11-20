#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run production analog route lifecycle functions with fake DSP/peripherals."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / "src/driver/PujjoAnalog.c").read_text()
online = source[source.index("static BOOLEAN RouteSessionOnline("):
                source.index("static VOID SetPositionAllowed(")]
stop = source[source.index("static NTSTATUS StopRoute("):
              source.index("static ULONG SspWord(")]
run = source[source.index("static NTSTATUS AnalogSetState("):
             source.index("static NTSTATUS AnalogJackPresent(")]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned ULONG;
typedef int BOOLEAN, NTSTATUS;
typedef void *PVOID;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_INVALID_PARAMETER -2
#define STATUS_DEVICE_NOT_CONNECTED -3
#define STATUS_TEST_FAILURE -4
#define NT_SUCCESS(s) ((s)>=0)
enum { PowerDeviceD0=1, SOF_FW_BOOT_COMPLETE=2 };
typedef enum { KSSTATE_STOP, KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN } KSSTATE;
enum { PujjoAudioHeadphone=1,PujjoAudioHeadsetMic,PujjoAudioSpeaker };
typedef enum { PJ5682S_JACK_ABSENT,PJ5682S_JACK_HEADSET,PJ5682S_JACK_PRESENT_UNCLASSIFIED } pj5682s_jack;
enum { Pj5682sJackClassify,Pj5682sJackPresence };
typedef struct { int DspMuted, GainLimitApplied; struct { int dsp_muted; } Codec; } PUJJO_PERIPHERAL_PROFILE;
typedef struct { int Role,MaximumPlaybackGain; PUJJO_PERIPHERAL_PROFILE Peripheral; } PUJJO_ANALOG_PROFILE;
typedef struct { int IsCapture; } STREAM;
typedef struct {
    struct { void *Context; } Header;
    int (*Prepare)(void *, void *, const PUJJO_PERIPHERAL_PROFILE *);
    int (*Shutdown)(void *, void *);
    int (*ReadJack)(void *, void *, int, pj5682s_jack *);
    int (*IsActive)(void *, void *);
} BUS;
typedef struct {
    STREAM *Stream; BUS Peripheral;
    int PeripheralNeedsShutdown,JackClassified,PositionAllowed,Faulted,Detached;
} PUJJO_ANALOG_LEASE;
typedef struct {
    int AnalogClosed,ArmedTesting,EnablePortCls,CurrentPowerState,FwState,AudioDmaFaulted;
    PUJJO_ANALOG_LEASE *AnalogLease;
} DEVICE, *PSOF_DEVICE_CONTEXT;
typedef struct { DEVICE *Parent; PUJJO_ANALOG_PROFILE Profile; } PUJJO_ANALOG_CHILD;
enum { MUTE=1,VOLUME,RUN,WAIT,PREPARE,JACK,UNMUTE,SHUTDOWN,STOP,PAUSE };
static int trace[64],count,fail_op,fail_rollback,active,drop_on_unmute,locked;
static pj5682s_jack jack;
static int record(int op) {
    assert(locked && count<64);trace[count++]=op;
    if(op==fail_op || (fail_rollback && op==SHUTDOWN)) return STATUS_TEST_FAILURE;
    return 0;
}
static void LockAudio(DEVICE *d) { assert(d && !locked);locked=1; }
static void UnlockAudio(DEVICE *d) { assert(d && locked);locked=0; }
static void SetPositionAllowed(DEVICE *d, PUJJO_ANALOG_LEASE *l, int allowed) {
    assert(d && l && locked); l->PositionAllowed=allowed;
}
static int SofWaveRtSetMute(STREAM *s,int mute) {
    assert(s);int rc=record(mute?MUTE:UNMUTE);
    if(!mute && drop_on_unmute) active=0;
    return rc;
}
static int SofWaveRtSetVolume(STREAM *s,int channel,int gain) {
    assert(s && !s->IsCapture && channel==-1 && gain==-100);return record(VOLUME);
}
static int SofWaveRtSetState(STREAM *s,KSSTATE state) {
    assert(s);return record(state==KSSTATE_RUN?RUN:state==KSSTATE_PAUSE?PAUSE:STOP);
}
static int WaitForHeadsetClock(const PUJJO_ANALOG_PROFILE *p) {
    return p->Role==PujjoAudioSpeaker?0:record(WAIT);
}
static int prepare(void *ctx,void *owner,const PUJJO_PERIPHERAL_PROFILE *p) {
    assert(ctx && owner && p->DspMuted==1 && p->Codec.dsp_muted==1 && p->GainLimitApplied==1);
    int rc=record(PREPARE);if(!rc)active=1;return rc;
}
static int shutdown(void *ctx,void *owner) {
    assert(ctx&&owner);int rc=record(SHUTDOWN);if(!rc)active=0;return rc;
}
static int readjack(void *ctx,void *owner,int query,pj5682s_jack *out) {
    assert(ctx&&owner&&(query==Pj5682sJackClassify||query==Pj5682sJackPresence));
    *out=jack;return record(JACK);
}
static int isactive(void *ctx,void *owner) { assert(ctx&&owner);return active; }
'''
suffix = r'''
static DEVICE d;
static PUJJO_ANALOG_CHILD child;
static PUJJO_ANALOG_LEASE lease;
static STREAM stream;
static void reset(int role) {
    memset(&d,0,sizeof(d));memset(&child,0,sizeof(child));memset(&lease,0,sizeof(lease));
    count=fail_op=fail_rollback=active=drop_on_unmute=locked=0;jack=PJ5682S_JACK_HEADSET;
    d.ArmedTesting=d.EnablePortCls=1;d.CurrentPowerState=PowerDeviceD0;d.FwState=SOF_FW_BOOT_COMPLETE;
    d.AnalogLease=&lease;child.Parent=&d;child.Profile.Role=role;child.Profile.MaximumPlaybackGain=-100;
    stream.IsCapture=role==PujjoAudioHeadsetMic;lease.Stream=&stream;
    lease.Peripheral=(BUS){{&d},prepare,shutdown,readjack,isactive};
}
static int call(ULONG state) { int rc=AnalogSetState(&child,&lease,state);assert(!locked);return rc; }
static void check_trace(const int *expected,int n) {
    assert(count==n);assert(!memcmp(trace,expected,(size_t)n*sizeof(int)));
}
int main(void) {
    const int speaker[]={MUTE,VOLUME,RUN,PREPARE,UNMUTE};
    const int headphone[]={MUTE,VOLUME,RUN,WAIT,PREPARE,JACK,UNMUTE};
    const int microphone[]={MUTE,RUN,WAIT,PREPARE,JACK,UNMUTE};
    const int stop_trace[]={MUTE,SHUTDOWN,STOP};
    for(int role=1;role<=3;++role) {
        const int *start=role==1?headphone:role==2?microphone:speaker;
        int n=role==1?7:role==2?6:5;
        reset(role);assert(call(KSSTATE_RUN)==0);check_trace(start,n);
        assert(active && lease.PositionAllowed && lease.PeripheralNeedsShutdown && !lease.Faulted);
        count=0;assert(call(KSSTATE_RUN)==0);assert(count==1 && trace[0]==RUN);
        count=0;d.AudioDmaFaulted=1;assert(call(KSSTATE_STOP)==0);check_trace(stop_trace,3);
        assert(!active&&!lease.PositionAllowed&&!lease.PeripheralNeedsShutdown);
        count=0;assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY && !count);
        for(int i=0;i<n;++i) {
            reset(role);fail_op=start[i];
            assert(call(KSSTATE_RUN)==STATUS_TEST_FAILURE);
            assert(!lease.PositionAllowed && lease.Faulted && trace[count-1]==STOP);
            assert(!active);
            if(start[i]==MUTE) assert(d.AudioDmaFaulted);
            else assert(!d.AudioDmaFaulted);
            count=0;assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY && !count);
        }
        reset(role);drop_on_unmute=1;assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);
        assert(!active&&!lease.PositionAllowed&&lease.Faulted&&trace[count-1]==STOP);
        reset(role);assert(call(KSSTATE_RUN)==0);count=0;fail_rollback=1;
        assert(call(KSSTATE_STOP)==STATUS_TEST_FAILURE);check_trace(stop_trace,3);
        assert(lease.Faulted && d.AudioDmaFaulted && !lease.PositionAllowed && lease.PeripheralNeedsShutdown);
        count=0;fail_rollback=0;assert(call(KSSTATE_STOP)==0);check_trace(stop_trace,3);
        assert(!active && !lease.PeripheralNeedsShutdown && d.AudioDmaFaulted);
        reset(role);lease.Detached=1;assert(call(KSSTATE_STOP)==STATUS_DEVICE_NOT_READY&&!count);
        reset(role);d.AnalogClosed=1;assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY&&!count);
        reset(role);assert(call(77)==STATUS_INVALID_PARAMETER&&!count);
    }
    reset(PujjoAudioHeadsetMic);jack=PJ5682S_JACK_PRESENT_UNCLASSIFIED;
    assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_CONNECTED&&!lease.PositionAllowed&&!active);
    reset(PujjoAudioHeadphone);jack=PJ5682S_JACK_ABSENT;
    assert(call(KSSTATE_RUN)==STATUS_DEVICE_NOT_CONNECTED&&!lease.PositionAllowed&&!active);
    puts("PASS: actual analog route start/stop, fault cleanup, jack qualification and removal recheck");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_analog_route_") as tmp:
    src=Path(tmp)/"route.c"
    exe=Path(tmp)/("route.exe" if os.name=="nt" else "route")
    src.write_text(prefix+online+stop+run+suffix)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=15)
