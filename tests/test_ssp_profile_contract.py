#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Guard the reviewed-blob and muted host-delay contracts without hardware."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1] / "src/driver"
source = (root / "PujjoAnalog.c").read_text()
header = (root / "PujjoAnalog.h").read_text()
assert "static const PUJJO_ANALOG_PROFILE *const approved[] = {NULL};" in source
allocate = source.split("static NTSTATUS AnalogAllocate", 1)[1].split("static VOID AnalogFree", 1)[0]
assert allocate.index("VerifySspConfiguration(parent, profile)") < allocate.index("OpenPeripheral(")
publish = source.split("static NTSTATUS PublishAnalogChildLocked", 1)[1]
assert publish.index("VerifySspConfiguration(parent, profile)") < publish.index("WdfPdoInitAllocate(")
run = source.split("if (lease->Faulted) goto Done;", 1)[1].split("Done:", 1)[0]
run = run[run.index("SetPositionAllowed(parent, lease, FALSE);"):]
assert run.index("SofWaveRtSetMute(lease->Stream, TRUE)") < run.index("SofWaveRtSetState(lease->Stream, KSSTATE_RUN)") < run.index("WaitForHeadsetClock(profile)") < run.index("lease->Peripheral.Prepare(") < run.index("SofWaveRtSetMute(lease->Stream, FALSE)")
assert "StopRoute(parent, lease, KSSTATE_STOP)" in run

# Compile the actual production helpers against host fixture types, not WDK.
helpers = source[source.index("static ULONG SspWord"):source.index("static NTSTATUS AnalogGetFormat")]
prefix = r'''
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t ULONG;
typedef unsigned char UCHAR;
typedef int BOOLEAN;
typedef long long LONGLONG;
typedef int NTSTATUS;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_NOT_SUPPORTED -2
#define STATUS_INVALID_BUFFER_SIZE -3
#define STATUS_INVALID_DEVICE_STATE -4
#define STATUS_INVALID_PARAMETER -5
#define NT_SUCCESS(s) ((s)>=0)
#define NHLT_LINK_SSP 3
#define NHLT_DIRECTION_CAPTURE 1
#define NHLT_DIRECTION_RENDER 0
#define NHLT_DEVICE_I2S 4
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096
#define PUJJO_REVIEWED_SSP_BYTES 96u
#define PUJJO_HEADSET_SETTLE_MAX_MS 500u
#define PASSIVE_LEVEL 0
#define KernelMode 0
#define RtlEqualMemory(a,b,n) (!memcmp(a,b,n))
enum { PujjoAudioHeadphone=1,PujjoAudioHeadsetMic=2,PujjoAudioSpeaker=3 };
struct sof_ipc4_copier_data { ULONG fields[16]; };
typedef struct { int Role; ULONG VerifiedSsp; UCHAR VerifiedSspConfig[96]; ULONG HeadsetClockSettleMs; struct { ULONG Rate,Channels,ContainerBits,ValidBits; } Format; } PUJJO_ANALOG_PROFILE;
typedef struct { void *NhltTable; ULONG NhltTableSize; } DEVICE, *PSOF_DEVICE_CONTEXT;
typedef struct { UCHAR LinkType,VirtualBusId,Direction,DeviceType; ULONG SampleRate,Channels,ContainerBits,ValidBits; } SOF_NHLT_CONFIG_KEY;
static UCHAR blob[96];
static ULONG length=96;
static int lookup_status=0, irql=0, waits=0, wait_status=0;
static long long interval_value;
static SOF_NHLT_CONFIG_KEY observed;
static int SofNhltCopyConfiguration(void *table, ULONG size, SOF_NHLT_CONFIG_KEY *key, UCHAR *out, ULONG capacity, ULONG *bytes) {
    assert(table && size && capacity==96); observed=*key; *bytes=length;
    if (lookup_status) return lookup_status;
    memcpy(out,blob,capacity); return 0;
}
static int KeGetCurrentIrql(void) { return irql; }
static int KeDelayExecutionThread(int mode, int alertable, LARGE_INTEGER *interval) {
    assert(mode==KernelMode && !alertable); ++waits; interval_value=interval->QuadPart; return wait_status;
}
'''
suffix = r'''
static void word(unsigned n, uint32_t v) {
    for (unsigned i=0;i<4;++i) blob[n*4+i]=(UCHAR)(v>>(8*i));
}
static void geometry(unsigned speaker) {
    memset(blob,0,sizeof(blob));
    word(1,0xee000105u); word(2,96); word(11,0x100007u | ((speaker?7u:15u)<<8));
    word(14,(speaker?32u:25u)<<16); word(15,3); word(16,3);
    word(21,1); word(22,1); word(23,0);
}
int main(void) {
    DEVICE d={blob,96};
    PUJJO_ANALOG_PROFILE p={0};
    p.Role=PujjoAudioHeadsetMic; p.Format.Rate=48000; p.Format.Channels=2; p.Format.ContainerBits=32; p.Format.ValidBits=24;
    assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
    geometry(0); memcpy(p.VerifiedSspConfig,blob,96);
    assert(VerifySspConfiguration(&d,&p)==0);
    assert(observed.LinkType==3 && observed.VirtualBusId==0 && observed.Direction==1 && observed.ValidBits==24);
    p.Role=PujjoAudioSpeaker; p.VerifiedSsp=1;
    assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
    geometry(1); memcpy(p.VerifiedSspConfig,blob,96);
    assert(VerifySspConfiguration(&d,&p)==0 && observed.VirtualBusId==1 && observed.Direction==0);
    word(14,25u<<16); memcpy(p.VerifiedSspConfig,blob,96);
    assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED);
    geometry(1); memcpy(p.VerifiedSspConfig,blob,96);
    blob[95]^=1; assert(VerifySspConfiguration(&d,&p)==STATUS_NOT_SUPPORTED); blob[95]^=1;
    length=95; assert(VerifySspConfiguration(&d,&p)==STATUS_INVALID_BUFFER_SIZE); length=96;
    lookup_status=-7; assert(VerifySspConfiguration(&d,&p)==-7); lookup_status=0;
    d.NhltTable=0; assert(VerifySspConfiguration(&d,&p)==STATUS_DEVICE_NOT_READY);
    assert(WaitForHeadsetClock(&p)==0 && waits==0);
    p.HeadsetClockSettleMs=1; assert(WaitForHeadsetClock(&p)==STATUS_NOT_SUPPORTED && waits==0);
    p.Role=PujjoAudioHeadphone;
    p.HeadsetClockSettleMs=0; assert(WaitForHeadsetClock(&p)==STATUS_NOT_SUPPORTED);
    p.HeadsetClockSettleMs=501; assert(WaitForHeadsetClock(&p)==STATUS_NOT_SUPPORTED);
    p.HeadsetClockSettleMs=25; irql=2; assert(WaitForHeadsetClock(&p)==STATUS_INVALID_DEVICE_STATE && waits==0);
    irql=0; assert(WaitForHeadsetClock(&p)==0 && waits==1 && interval_value==-250000);
    wait_status=-8; assert(WaitForHeadsetClock(&p)==-8 && waits==2);
    return 0;
}
'''
assert "#define PUJJO_REVIEWED_SSP_BYTES 96u" in header
assert "#define PUJJO_HEADSET_SETTLE_MAX_MS 500u" in header
with tempfile.TemporaryDirectory() as temp:
    source_file = Path(temp) / "ssp.c"
    binary = Path(temp) / "ssp"
    source_file.write_text(prefix + helpers + suffix)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(source_file), "-o", str(binary)], check=True, timeout=40)
    subprocess.run([str(binary)], check=True, timeout=20)
print("PASS: exact reviewed SSP bytes, direction and fail-closed host delay (host fixtures only)")
