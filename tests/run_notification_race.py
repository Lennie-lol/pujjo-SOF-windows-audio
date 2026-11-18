"""Exercise WaveRT event registration and DPC signaling with a host lock double."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/driver/WaveRt.c").read_text()
functions = []
for declaration in ("NTSTATUS SofWaveRtRegisterNotificationEvent",
                    "NTSTATUS SofWaveRtUnregisterNotificationEvent",
                    "VOID SofWaveRtSignalNotification"):
    start = source.index(declaration + "(")
    functions.append(source[start:source.index("\n}", start) + 2])

shim = """
#include <threads.h>
#include <stdatomic.h>
#include <assert.h>
#include <stddef.h>
#define _KERNEL_MODE
#define _In_
#define VOID void
#define FALSE 0
#define SOF_WAVERT_MAX_EVENTS 8
#define STATUS_SUCCESS 0
#define STATUS_NOT_FOUND -1
#define STATUS_INSUFFICIENT_RESOURCES -2
#define STATUS_INVALID_PARAMETER -3
typedef int NTSTATUS, KIRQL;
typedef unsigned long ULONG;
typedef void *PVOID;
typedef struct { atomic_int registered; unsigned calls; } event, *PKEVENT;
typedef struct { mtx_t NotificationLock; ULONG NumNotificationEvents;
                 PKEVENT NotificationEvents[8]; } stream, *PSOF_WAVERT_STREAM;
#define KeAcquireSpinLock(lock,irql) ((void)(irql),mtx_lock(lock))
#define KeReleaseSpinLock(lock,irql) ((void)sizeof(irql),mtx_unlock(lock))
#define KeSetEvent(e,inc,wait) record(e)
static void record(PKEVENT e) { assert(atomic_load(&e->registered)); ++e->calls; }
"""
cases = """
static stream s;
static atomic_int done;
static int signal_loop(void *unused) {
    (void)unused;
    while (!atomic_load(&done)) SofWaveRtSignalNotification(&s);
    return 0;
}
static int cycle(void *arg) {
    event *e = arg;
    for (unsigned i=0; i<5000; ++i) {
        atomic_store(&e->registered, 1);
        assert(SofWaveRtRegisterNotificationEvent(&s,e)==0);
        assert(SofWaveRtRegisterNotificationEvent(&s,e)==0);
        SofWaveRtSignalNotification(&s);
        assert(SofWaveRtUnregisterNotificationEvent(&s,e)==0);
        atomic_store(&e->registered, 0);
        assert(SofWaveRtUnregisterNotificationEvent(&s,e)==STATUS_NOT_FOUND);
    }
    return 0;
}
int main(void) {
    event events[4]={0}; thrd_t signal, workers[4];
    assert(mtx_init(&s.NotificationLock,mtx_plain)==thrd_success);
    assert(thrd_create(&signal,signal_loop,NULL)==thrd_success);
    for(unsigned i=0;i<4;++i) assert(thrd_create(&workers[i],cycle,&events[i])==thrd_success);
    for(unsigned i=0;i<4;++i) { thrd_join(workers[i],NULL); assert(events[i].calls>=5000); }
    atomic_store(&done,1); thrd_join(signal,NULL);
    assert(s.NumNotificationEvents==0); mtx_destroy(&s.NotificationLock);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "notification.c"
    path.write_text("\n".join([shim, *functions, cases]))
    binary = path.with_suffix("")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-Wall",
                    "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
