"""Execute the production capture conversion with controlled DMA cursor/time samples."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/driver/WaveRt.c").read_text()
start = source.index("static BOOLEAN SofWaveRtCopyVoicePeriod(")
function = source[start:source.index("\n}", start) + 2]
fixture = r"""
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int16_t SHORT;
typedef unsigned char *PUCHAR;
typedef int BOOLEAN, KIRQL;
#define KeAcquireSpinLock(lock,irql) ((void)(lock), *(irql)=0)
#define KeReleaseSpinLock(lock,irql) ((void)(lock), (void)(irql))
#define PJ_DMIC_RATE 48000u
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define KeMemoryBarrier() ((void)0)
#define ROUND_TO_PAGES(n) (((n)+4095u)&~4095u)
#define InterlockedExchange(p,v) (*(p)=(v))
#define InterlockedCompareExchange(p,x,y) (*(p))
#define RtlCopyMemory memcpy
typedef struct {
 ULONG PeriodSizeBytes,BufferSizeBytes,PeriodMs;
 LONG VoicePosition;
 LONG *KernelBuffer;
 SHORT *VoiceBuffer,*VoiceScratch;
 ULONGLONG VoiceLastCompletion, VoicePacketCount, VoiceTimestamp[2], CounterFrequency;
 int NotificationLock;
 ULONG VoiceBufferBytes;
} Stream,*PSOF_WAVERT_STREAM;
static ULONG first,last,reads;
static ULONGLONG now,delay;
static ULONGLONG KeQueryInterruptTimePrecise(ULONGLONG *q) {
 *q=now; now+=delay; return *q;
}
static int SofWaveRtGetPosition(Stream *s, ULONG *pos) {
 *pos=reads++ ? last : first;
 return *pos<s->BufferSizeBytes ? 0 : -1;
}
"""
cases = r"""
int main(void) {
 LONG slots[16]={INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX,
                 INT32_MIN,INT32_MIN,INT32_MIN,INT32_MIN,
                 65536,131072,196608,262144,0,0,0,0};
 SHORT pcm[4]={0},scratch[2]={0};
 Stream s={32,64,20,0,slots,pcm,scratch,0,0,{0},48000,0,8};
 now=100; first=32; last=48; delay=1;
 assert(SofWaveRtCopyVoicePeriod(&s) && s.VoicePosition==4);
 assert(s.VoicePacketCount==1 && s.VoiceTimestamp[0]==98);
 assert(pcm[0]==32767 && pcm[1]==-32768);
 for(int i=0;i<5;++i) {
   reads=0;first=0;last=4;delay=1;now=1010;
   if(i==0) first=32; /* same half as destination */
   if(i==1) last=32; /* DMA crossed into source during copy */
   if(i==2) last=UINT32_MAX;
   if(i==3) delay=200000; /* wrapped back while copying */
   if(i==4) now=400100; /* no trustworthy completion count */
   assert(!SofWaveRtCopyVoicePeriod(&s) && s.VoicePosition==4 && pcm[2]==0);
 }
 now=1020;delay=1;reads=0;first=0;last=4;
 assert(SofWaveRtCopyVoicePeriod(&s) && !s.VoicePosition && pcm[2]==2 && pcm[3]==0);
 assert(s.VoicePacketCount==2 && s.VoiceTimestamp[1]==1018);
 /* Internally consistent DMA sizes must still fit the attached CPU lease. */
 s.PeriodSizeBytes=64;s.BufferSizeBytes=128;reads=0;
 assert(!SofWaveRtCopyVoicePeriod(&s) && !reads && s.VoicePacketCount==2);
 s.PeriodSizeBytes=32;s.BufferSizeBytes=64;
 /* Corrupted geometry and missing private scratch cannot publish. */
 s.VoicePosition=2;assert(!SofWaveRtCopyVoicePeriod(&s));
 s.VoicePosition=0;s.VoiceScratch=0;assert(!SofWaveRtCopyVoicePeriod(&s));
 s.VoiceScratch=scratch;s.BufferSizeBytes=65;assert(!SofWaveRtCopyVoicePeriod(&s));
 assert(s.VoicePacketCount==2);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "voice.c"
    path.write_text("\n".join([fixture, function, cases]))
    binary = path.with_suffix("")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
