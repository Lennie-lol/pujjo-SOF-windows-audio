"""Run the diagnostic prepare guard with competing device instances on a host."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/driver/Device.c").read_text()
prepare = source.split("SofEvtDevicePrepareHardware(", 1)[1]
guard = prepare.split("#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC", 1)[1].split("#endif", 1)[0]
driver = (root / "src/driver/Driver.c").read_text()
# Only a successful DriverEntry can publish a permit. PnP cannot renew it.
assert driver.count("g_DiagnosticPreparePermit") == 2
assert source.count("g_DiagnosticPreparePermit") == 1
assert driver.index("g_CanaryArmed = SofCommitDiagnosticDisarm(") < driver.index("if (g_CanaryArmed) {") < driver.index("InterlockedExchange(&g_DiagnosticPreparePermit, 1)")
for unit in (root / "src/driver").rglob("*.c"):
    assert "->ReferenceCount" not in unit.read_text(), unit
program = r"""
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#define SOF_SAFE_BOOT_SINGLE_SHOT 1
#define STATUS_NOT_SUPPORTED -1
static int g_SafeBootPolicy = 1;
static atomic_int g_DiagnosticPreparePermit, discovered;
static pthread_barrier_t start;
static int InterlockedCompareExchange(atomic_int *target, int value, int expected) {
    atomic_compare_exchange_strong(target, &expected, value);
    return expected;
}
static int prepare(int fail) {
""" + guard + r"""
    atomic_fetch_add(&discovered, 1);
    return fail ? -2 : 0;
}
static void *start_device(void *fail) {
    pthread_barrier_wait(&start);
    prepare(*(int *)fail);
    return NULL;
}
int main(void) {
    for (int fail = 0; fail <= 1; ++fail) {
        pthread_t threads[16];
        atomic_store(&g_DiagnosticPreparePermit, 1);
        atomic_store(&discovered, 0);
        assert(!pthread_barrier_init(&start, NULL, 16));
        for (int i = 0; i < 16; ++i)
            assert(!pthread_create(&threads[i], NULL, start_device, &fail));
        for (int i = 0; i < 16; ++i) assert(!pthread_join(threads[i], NULL));
        pthread_barrier_destroy(&start);
        assert(discovered == 1);
        /* Rebalanced device and fresh instances after removal share the permit. */
        for (int i = 0; i < 32; ++i) assert(prepare(0) == STATUS_NOT_SUPPORTED);
        assert(discovered == 1);
    }
}
"""
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "prepare"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-pthread",
                    "-x", "c", "-", "-o", exe], input=program, text=True, check=True)
    subprocess.run([exe], check=True, timeout=30)
