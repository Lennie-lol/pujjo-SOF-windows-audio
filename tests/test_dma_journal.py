#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise the actual registry journal and atomic descriptor ledger."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from dma_publication_fixture import ledger, loader, driver

driver_source = (driver/"Driver.c").read_text()
device_source = (driver/"Device.c").read_text()
workflow = (driver.parents[1]/".github/workflows/compile-windows.yml").read_text()
entry = driver_source.split("DriverEntry(\n",1)[1].split("#else",1)[0]
assert "PUJJO_DMIC_ENUMERATION_CANDIDATE" in entry and "STATUS_NOT_SUPPORTED" in entry
assert "dmic-rom-boot-compile-only" in workflow
assert "guarded-dmic-rom-boot-compile-only/bin" not in workflow
gate = loader.split("static BOOLEAN SofPlatformDmaQualified(",1)[1].split("\n}",1)[0]
assert "return FALSE;" in gate
boot = loader.split("NTSTATUS\nSofStartFirmwareBoot(",1)[1]
assert boot.index("SofPlatformDmaQualified(Ctx)") < boot.index("SofBeginDmaJournal(Ctx)") < boot.index("SofAllocateOwnedDmaBuffer(") < boot.index("SofAcquireBootProcessing(Ctx)")
candidate = device_source.split("#ifdef PUJJO_DMIC_ROM_BOOT_CANDIDATE",1)[1].split("#else",1)[0]
assert candidate.index("SofCloseDmaPublicationGate") < candidate.index("SofIpcDisableInterrupts") < candidate.index("SofIpcQuiesce") < candidate.index("SofPowerDownDsp") < candidate.index("SofRestoreBootController") < candidate.index("return STATUS_SUCCESS")
for forbidden in ("SofPublishAudioChild", "SofPublishApprovedAnalogChildren", "WdfTimerStart"):
    assert forbidden not in candidate
assert driver_source.count('L"DmaSessionDirty"') == 1
assert "DmaSessionDirty" not in device_source
assert all("DmaSessionDirty" not in inf.read_text(errors="replace") for inf in driver.parents[1].rglob("*.inf"))
journal = driver_source[driver_source.index("static volatile LONG g_DmaJournalAttempted;"):
                         driver_source.index("#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC\n/* The diagnostic never interprets")]
shim = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <pthread.h>
typedef uint32_t ULONG;
typedef int BOOLEAN, NTSTATUS;
typedef void VOID;
typedef struct { int HardwarePrepared, Removed, BootDmaStopUnconfirmed, AudioDmaFaulted;
 volatile int32_t DmaPublicationState; } CTX, *PSOF_DEVICE_CONTEXT;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_DEVICE_NOT_READY -2
#define KEY_READ 1
#define KEY_WRITE 2
#define WDF_NO_OBJECT_ATTRIBUTES 0
#define PASSIVE_LEVEL 0
typedef void *WDFKEY;
typedef const wchar_t *UNICODE_STRING;
static int SofDeviceRemoved(CTX *c) {return c->Removed;}
'''
registry = r'''
static int phase, opened, closed, queries, writes, flushes, failure, apcs, irql;
static ULONG persisted;
static CTX *invalidateDuringFlush;
static int KeGetCurrentIrql(void) {return irql;}
static int KeAreAllApcsDisabled(void) {return apcs;}
static void RtlInitUnicodeString(UNICODE_STRING *name, const wchar_t *v) {*name=v;}
static void *WdfGetDriver(void) {return NULL;}
static NTSTATUS WdfDriverOpenParametersRegistryKey(void *d,int access,int attr,WDFKEY *key) {
 assert(!d && access==(KEY_READ|KEY_WRITE) && !attr);
 ++opened; phase=1;
 if(failure==1) return -5;
 if(failure==8) return 1;
 *key=&persisted; return 0;
}
static NTSTATUS WdfRegistryQueryULong(WDFKEY k,UNICODE_STRING *n,ULONG *v) {
 assert(k==&persisted && !wcscmp(*n,L"DmaSessionDirty"));
 ++queries;
 assert(phase==1 || phase==3);
 if(failure==2 && phase==1) return -5;
 if(failure==5 && phase==3) return -5;
 *v=(failure==6 && phase==3)?0:persisted;
 return 0;
}
static NTSTATUS WdfRegistryAssignULong(WDFKEY k,UNICODE_STRING *n,ULONG v) {
 assert(k==&persisted && !wcscmp(*n,L"DmaSessionDirty") && v==1 && phase==1);
 ++writes; phase=2;
 if(failure==3) return -5;
 persisted=v; return 0;
}
static void *WdfRegistryWdmGetHandle(WDFKEY k) {return k;}
static NTSTATUS ZwFlushKey(void *k) {
 assert(k==&persisted && phase==2); ++flushes; phase=3;
 if(invalidateDuringFlush) SofInvalidateDmaPublicationGate(invalidateDuringFlush);
 if(failure==4) return -5;
 if(failure==7) return 1;
 return 0;
}
static void WdfRegistryClose(WDFKEY k) {assert(k==&persisted); ++closed;}
#define SofLogBreadcrumb(s) ((void)(s))
'''
cases = r'''
static void reset(void) {
 /* Test fixture simulates a new driver image; production never resets. */
 g_DmaJournalAttempted=phase=opened=closed=queries=writes=flushes=failure=apcs=irql=0;
 persisted=0; invalidateDuringFlush=NULL;
}
typedef struct {CTX *c; unsigned slot; int result;} Work;
static void *journal_worker(void *p) {Work *w=p; w->result=SofBeginDmaJournal(w->c)==0; return NULL;}
static void *claim_worker(void *p) {Work *w=p; w->result=SofClaimDmaPublication(w->c,w->slot); return NULL;}
static void *close_worker(void *p) {SofCloseDmaPublicationGate(p);return NULL;}
static void *invalidate_worker(void *p) {SofInvalidateDmaPublicationGate(p);return NULL;}
static void *detach_worker(void *p) {Work *w=p;w->result=SofConfirmDmaDetached(w->c,w->slot);return NULL;}
int main(void) {
 for(int mode=1;mode<=8;++mode) {
   reset(); CTX c={.HardwarePrepared=1}; failure=mode;
   assert(SofBeginDmaJournal(&c)<0);
   assert(!SofDmaPublicationGateOpen(&c) && !SofClaimDmaPublication(&c,0));
   int before=opened; assert(SofBeginDmaJournal(&c)<0 && opened==before);
   assert(closed==(mode!=1 && mode!=8));
   if(mode>=4 && mode<=7) assert(persisted==1);
 }
 reset(); CTX c={.HardwarePrepared=1};
 persisted=1;
 assert(SofBeginDmaJournal(&c)<0 && !writes && !flushes);
 reset(); c.DmaPublicationState=0; invalidateDuringFlush=&c;
 assert(SofBeginDmaJournal(&c)<0 && persisted==1 && !SofDmaPublicationGateOpen(&c));
 assert(!SofOpenDmaPublicationGate(&c));
 reset(); c.DmaPublicationState=0; apcs=1;
 assert(SofBeginDmaJournal(&c)<0 && !opened);
 apcs=0; irql=2; assert(SofBeginDmaJournal(&c)<0 && !opened);
 reset();
 CTX others[16]={0}; Work jobs[16]; pthread_t threads[16];
 for(unsigned i=0;i<16;++i) {
   others[i].HardwarePrepared=1; jobs[i]=(Work){&others[i],5,0};
   assert(!pthread_create(&threads[i],NULL,journal_worker,&jobs[i]));
 }
 unsigned winners=0;
 for(unsigned i=0;i<16;++i) {
   assert(!pthread_join(threads[i],NULL)); winners+=jobs[i].result;
   assert(SofDmaPublicationGateOpen(&others[i])==jobs[i].result);
 }
 assert(winners==1 && opened==1 && closed==1 && persisted==1 && queries==2 && writes==1 && flushes==1);
 /* A fresh driver after unload must still reject the persistent dirty word. */
 g_DmaJournalAttempted=0; c.DmaPublicationState=0;
 assert(SofBeginDmaJournal(&c)<0 && !SofDmaPublicationGateOpen(&c));
 c.DmaPublicationState=0; assert(SofOpenDmaPublicationGate(&c)); /* test-only journal model */
 assert(!SofClaimDmaPublication(&c,30));
 for(unsigned i=0;i<16;++i) {
   jobs[i]=(Work){&c,7,0}; assert(!pthread_create(&threads[i],NULL,claim_worker,&jobs[i]));
 }
 winners=0;
 for(unsigned i=0;i<16;++i) {assert(!pthread_join(threads[i],NULL));winners+=jobs[i].result;}
 assert(winners==1 && SofDmaPublishedSlots(&c)==(1u<<7));
 SofCloseDmaPublicationGate(&c);
 assert(!SofClaimDmaPublication(&c,2) && !SofOpenDmaPublicationGate(&c));
 assert(SofDmaDetachLeaseOwned(&c,7) && SofConfirmDmaDetached(&c,7));
 assert(!SofDmaPublishedSlots(&c) && !SofConfirmDmaDetached(&c,7));
 for(unsigned i=0;i<100;++i) {
   c.DmaPublicationState=0; assert(SofOpenDmaPublicationGate(&c));
   Work w={&c,29,0}; pthread_t a,b;
   assert(!pthread_create(&a,NULL,claim_worker,&w));
   assert(!pthread_create(&b,NULL,close_worker,&c));
   assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
   assert(!SofDmaPublicationGateOpen(&c) && !SofClaimDmaPublication(&c,0));
   assert(SofDmaPublishedSlots(&c)==(w.result?(1u<<29):0));
   if(w.result) assert(SofConfirmDmaDetached(&c,29));
   c.DmaPublicationState=0; assert(SofOpenDmaPublicationGate(&c));
   assert(SofClaimDmaPublication(&c,4)); w=(Work){&c,4,0};
   assert(!pthread_create(&a,NULL,detach_worker,&w));
   assert(!pthread_create(&b,NULL,invalidate_worker,&c));
   assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
   assert(!SofDmaDetachLeaseOwned(&c,4) && !SofConfirmDmaDetached(&c,4));
   assert(SofDmaPublishedSlots(&c)==(w.result?0:16));
 }
 c.DmaPublicationState=0; assert(SofOpenDmaPublicationGate(&c));
 assert(SofClaimDmaPublication(&c,0)); SofInvalidateDmaPublicationGate(&c);
 assert(!SofDmaDetachLeaseOwned(&c,0) && !SofConfirmDmaDetached(&c,0));
 assert(SofDmaPublishedSlots(&c)==1 && !SofOpenDmaPublicationGate(&c));
 puts("PASS: durable journal fault matrix, 16 competing starts, duplicate slot, close/claim and surprise/detach races");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/"journal.c"; exe=Path(tmp)/("journal.exe" if os.name=="nt" else "journal")
    src.write_text(shim+ledger+registry+journal+cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror","-pthread",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=20)
