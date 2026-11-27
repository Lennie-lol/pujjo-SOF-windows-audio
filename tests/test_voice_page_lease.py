#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute production PortCls buffer methods with an instrumented page/parent lease mock."""
from pathlib import Path
import os, shlex, subprocess, tempfile

root=Path(__file__).resolve().parents[1]
port=(root/"src/driver/PortClsAdapter.cpp").read_text()
bus=(root/"src/driver/PujjoAudioBus.c").read_text()
wave=(root/"src/driver/WaveRt.c").read_text()
start=port.index("    STDMETHODIMP AllocateBufferWithNotification(")
body=port[start:port.index("    NTSTATUS ChangeEvent(",start)]
assert "PUJJO_AUDIO_PACKET_VERSION 5" in (root/"src/driver/PujjoAudioBus.h").read_text()
assert "bus.PlanVoice = AudioPlanVoice" in bus and "bus.AttachVoice = AudioAttachVoice" in bus
assert "stream->VoiceBuffer = (SHORT *)VoiceAddress" in bus
assert "stream->VoiceScratch = ExAllocatePool2" in bus
assert "ExFreePoolWithTag(Stream->VoiceBuffer" not in wave
free=wave.split("NTSTATUS SofWaveRtFreeStream(",1)[1].split("NTSTATUS SofWaveRtDetachStream(",1)[0]
assert free.index("KeFlushQueuedDpcs()") < free.index("Stream->VoiceBuffer = NULL")
assert free.index("KeFlushQueuedDpcs()") < free.index("ExFreePoolWithTag(Stream,")
assert "m_bus.Header.Version != PUJJO_AUDIO_PACKET_VERSION" in port
fixture=r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <climits>
using ULONG=uint32_t; using PULONG=ULONG*; using NTSTATUS=int;
using PVOID=void*; using KIRQL=int;
enum MEMORY_CACHING_TYPE { MmCached=1 };
struct MDL { ULONG offset,bytes; MDL *Next; };
using PMDL=MDL*;
struct PHYSICAL_ADDRESS { int64_t QuadPart; };
#define MAXLONGLONG INT64_MAX
#define ROUND_TO_PAGES(n) (((n)+4095u)&~4095u)
#define MmGetMdlByteOffset(m) ((m)->offset)
#define MmGetMdlByteCount(m) ((m)->bytes)
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define KeAcquireSpinLock(l,i) ((void)(l),*(i)=0)
#define KeReleaseSpinLock(l,i) ((void)(l),(void)(i))
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define STDMETHODIMP NTSTATUS
#define STDMETHODIMP_(x) x
#define STATUS_DEVICE_BUSY -1
#define STATUS_INVALID_PARAMETER -2
#define STATUS_NOT_SUPPORTED -3
#define STATUS_INVALID_BUFFER_SIZE -4
#define STATUS_INSUFFICIENT_RESOURCES -5
#define STATUS_INVALID_DEVICE_STATE -6
#define NOTIFICATION_COUNT 2
#define VOID void
struct PUJJO_AUDIO_BUFFER { PVOID Stream; PMDL Mdl; ULONG Bytes; MEMORY_CACHING_TYPE CacheType; };
enum RoleType {PujjoAudioDmic};
struct Format {RoleType Role=PujjoAudioDmic;};
static int fail, pages, mapping, lease, held, calls;
static MDL mdl{};
alignas(4096) static unsigned char storage[12288];
struct Port {
 PMDL AllocatePagesForMdl(PHYSICAL_ADDRESS high,size_t count) {
   assert(held && !pages && high.QuadPart>0); ++calls;
   if(fail==2)return nullptr;
   pages=1;mdl={0,static_cast<ULONG>(count),nullptr};
   if(fail==3)mdl.bytes-=4096;
   if(fail==8)mdl.offset=1;
   if(fail==9)mdl.Next=&mdl;
   memset(storage,0x7e,sizeof(storage));return &mdl;
 }
 PVOID MapAllocatedPages(PMDL m,MEMORY_CACHING_TYPE cache) {
   assert(held && pages && !mapping && m==&mdl && cache==MmCached); ++calls;
   if(fail==4)return nullptr;
   mapping=1;return storage;
 }
 void UnmapAllocatedPages(PVOID p,PMDL m) {
   assert(held && mapping && !lease && p==storage && m==&mdl);mapping=0;++calls;
 }
 void FreePagesFromMdl(PMDL m) {
   assert(held && pages && !mapping && !lease && m==&mdl);pages=0;++calls;
 }
};
struct Bus {
 struct {PVOID Context;} Header={nullptr};
 NTSTATUS PlanVoice(PVOID,ULONG req,PULONG bytes) {
   assert(held);++calls;
   if(fail==1)return -10;
   *bytes=fail==7?0:req<3840?3840:req;return 0;
 }
 NTSTATUS AttachVoice(PVOID,ULONG,PVOID addr,ULONG bytes,PVOID *stream) {
   assert(held && mapping && pages && addr==storage);++calls;
   for(ULONG i=0;i<ROUND_TO_PAGES(bytes);i++)assert(!storage[i]);
   if(fail==5)return -11;
   if(fail==6)return 0; // malformed success without lease
   lease=1;*stream=&lease;return 0;
 }
 NTSTATUS Allocate(PVOID,ULONG,PUJJO_AUDIO_BUFFER*) {assert(false);return -1;}
 void Free(PVOID,PVOID s) {assert(held && s==&lease && lease && mapping);lease=0;++calls;}
};
struct Base {
 virtual NTSTATUS AllocateBufferWithNotification(ULONG,ULONG,PMDL*,PULONG,PULONG,MEMORY_CACHING_TYPE*)=0;
 virtual void FreeBufferWithNotification(PMDL,ULONG)=0;
 virtual ~Base()=default;
};
class Stream : public Base {
 public:
 PUJJO_AUDIO_BUFFER m_buffer={}; Format m_format; Bus m_bus; Port *m_port;
 PVOID m_voiceMapping=nullptr; int m_bufferLock=0;
 explicit Stream(Port *port):m_port(port){}
 void Lock(){assert(!held);held=1;}
 void Unlock(){assert(held);held=0;}
"""
cases=r"""
};
int main() {
 Port port; Stream s(&port); PMDL out=nullptr; ULONG actual=0,offset=0; MEMORY_CACHING_TYPE cache=MmCached;
 for(fail=1;fail<=9;fail++){
    assert(s.AllocateBufferWithNotification(2,3840,&out,&actual,&offset,&cache)<0);
    assert(!pages&&!mapping&&!lease&&!held&&!out&&!actual&&!s.m_buffer.Stream&&!s.m_voiceMapping);
 }
 fail=0;assert(!s.AllocateBufferWithNotification(2,320,&out,&actual,&offset,&cache));
 assert(pages&&mapping&&lease&&!held&&out==&mdl&&actual==3840&&!offset);
 int before=calls;PMDL other=nullptr;ULONG ignored=0;
 assert(s.AllocateBufferWithNotification(2,3840,&other,&ignored,&offset,&cache)==STATUS_DEVICE_BUSY);
 assert(calls==before);
 s.FreeBufferWithNotification(nullptr,0);assert(pages&&lease&&mapping);
 s.FreeBufferWithNotification(out,actual);assert(!pages&&!mapping&&!lease&&!held);
 before=calls;s.FreeBufferWithNotification(out,actual);assert(calls==before);
 assert(!s.AllocateBufferWithNotification(2,9600,&out,&actual,&offset,&cache));
 assert(lease&&actual==9600);
 s.FreeBufferWithNotification(out,actual);assert(!pages&&!mapping&&!lease);
}
"""
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/"test.cpp";p.write_text(fixture+body+cases)
    exe=p.with_suffix("")
    cc=os.environ.get("CC","gcc")
    cxx=os.environ.get("CXX", "clang++" if "clang" in cc else "g++")
    subprocess.run([cxx,"-std=c++17","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
print("PASS: production PortCls voice page allocation, partial/malformed MDL rollback, synchronous parent drain and repeated free")
