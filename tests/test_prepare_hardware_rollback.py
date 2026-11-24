#!/usr/bin/env python3
"""Run the production PCI selector and mapping rollback against host doubles."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/driver/Device.c").read_text()
prep = source.split("SofEvtDevicePrepareHardware(", 1)[1].split("\n/**", 1)[0]
release = source.split("SofEvtDeviceReleaseHardware(", 1)[1].split("\n/**", 1)[0]
assert prep.index("SofSelectPciResources(") < prep.index("SofMapPreparedBars(")
assert prep.index("if (!NT_SUCCESS(status)) goto PreparationFailed;", prep.index("SofMapPreparedBars(")) < prep.index("WdfInterruptCreate(")
assert prep.index("interruptConfig.InterruptRaw = interruptRaw;") < prep.index("WdfInterruptCreate(")
assert prep.index("interruptConfig.InterruptTranslated = interruptTranslated;") < prep.index("WdfInterruptCreate(")
assert prep.index("WdfInterruptCreate(") < prep.index("SofDiscoverHdaCaps(ctx)", prep.index("WdfInterruptCreate("))
assert "SofUnmapPreparedBars(ctx);" in prep.split("PreparationFailed:", 1)[1]
assert release.index("KeFlushQueuedDpcs()") < release.index("SofIpcCleanup(ctx)") < release.index("SofUnmapPreparedBars(ctx)", release.index("SofIpcCleanup(ctx)"))
assert "SofPowerDownDsp(" not in release
assert "ctx->BootDmaStopUnconfirmed || ctx->DspNeedsPowerDown" in release

mapping = source[source.index("static VOID SofUnmapPreparedBars("):source.index("/**\n * @brief Traverses")]
selector = source[source.index("static BOOLEAN SofMemoryRangeValid("):source.index("/**\n * @brief KMDF EvtDevicePrepareHardware")]
fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int BOOLEAN, NTSTATUS;
typedef void VOID;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
#define _Inout_
#define MAXLONGLONG INT64_MAX
#define PAGE_SIZE 4096
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_CONFIGURATION_ERROR -1
#define STATUS_INSUFFICIENT_RESOURCES -2
enum { CmResourceTypeMemory = 1, CmResourceTypeInterrupt, CmResourceTypeMemoryLarge };
enum { CM_RESOURCE_MEMORY_READ_ONLY = 1, CM_RESOURCE_MEMORY_WRITE_ONLY = 2,
       CM_RESOURCE_INTERRUPT_LATCHED = 1, CM_RESOURCE_INTERRUPT_MESSAGE = 2 };
typedef struct {
    unsigned Type, Flags;
    union {
        struct { PHYSICAL_ADDRESS Start; ULONG Length; } Memory;
        struct { struct { unsigned MessageCount; } Raw; } MessageInterrupt;
    } u;
} DESC, *PCM_PARTIAL_RESOURCE_DESCRIPTOR;
typedef struct { ULONG count; DESC descriptors[5]; } LIST, *WDFCMRESLIST;
static ULONG WdfCmResourceListGetCount(WDFCMRESLIST list) { return list->count; }
static DESC *WdfCmResourceListGetDescriptor(WDFCMRESLIST list, ULONG index)
{
    assert(index < list->count);
    return &list->descriptors[index];
}
typedef struct {
    PHYSICAL_ADDRESS Bar0PhysicalAddress, Bar4PhysicalAddress;
    ULONG Bar0Length, Bar4Length;
    void *Bar0VirtualAddress, *Bar4VirtualAddress;
} CTX, *PSOF_DEVICE_CONTEXT;
static ULONG maps, unmaps, fail_on;
static unsigned char mappings[2];
static PHYSICAL_ADDRESS physical[2];
static ULONG lengths[2];
#define MmNonCached 0
static void *MmMapIoSpace(PHYSICAL_ADDRESS address, ULONG length, int cache)
{
    assert(cache == MmNonCached && maps < 2);
    physical[maps] = address;
    lengths[maps] = length;
    ++maps;
    return maps == fail_on ? NULL : &mappings[maps - 1];
}
static void MmUnmapIoSpace(void *address, ULONG length)
{
    ULONG index = (address == &mappings[1]) ? 1 : 0;
    assert(address == &mappings[index] && length == lengths[index]);
    assert(unmaps++ < maps);
}
"""
cases = r"""
static LIST raw, translated;
static PHYSICAL_ADDRESS bar0 = {0xDEEAC000}, bar4 = {0xDEF00000};
static DESC *bars[2], *irq_raw, *irq_translated;

static void init(void)
{
    raw = (LIST){.count=4, .descriptors={
        {.Type=CmResourceTypeMemory, .u.Memory={bar4, 0x100000}},
        {.Type=CmResourceTypeInterrupt},
        {0},
        {.Type=CmResourceTypeMemory, .u.Memory={bar0, 0x4000}},
    }};
    translated = raw;
    translated.descriptors[0].u.Memory.Start.QuadPart = 0x100100000;
    translated.descriptors[3].u.Memory.Start.QuadPart = 0x100006000;
}

static NTSTATUS select_resources(void)
{
    bars[0] = bars[1] = irq_raw = irq_translated = NULL;
    return SofSelectPciResources(&raw, &translated, bar0, bar4, bars,
                                &irq_raw, &irq_translated);
}

int main(void)
{
    CTX ctx = {0};
    init();
    assert(select_resources() == STATUS_SUCCESS);
    assert(bars[0] == &translated.descriptors[3] && bars[1] == &translated.descriptors[0]);
    assert(irq_raw == &raw.descriptors[1] && irq_translated == &translated.descriptors[1]);

    for (fail_on = 1; fail_on <= 3; ++fail_on) {
        maps = unmaps = 0;
        assert(SofMapPreparedBars(&ctx, bars) ==
            (fail_on <= 2 ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS));
        assert(physical[0].QuadPart == 0x100006000 && lengths[0] == 0x4000);
        assert(maps == (fail_on == 1 ? 1u : 2u));
        if (maps == 2)
            assert(physical[1].QuadPart == 0x100100000 && lengths[1] == 0x100000);
        SofUnmapPreparedBars(&ctx);
        SofUnmapPreparedBars(&ctx);
        assert(unmaps == fail_on - 1);
        assert(!ctx.Bar0VirtualAddress && !ctx.Bar4VirtualAddress && !ctx.Bar0Length &&
               !ctx.Bar4Length && !ctx.Bar0PhysicalAddress.QuadPart && !ctx.Bar4PhysicalAddress.QuadPart);
    }

    raw.descriptors[1].Flags = translated.descriptors[1].Flags = CM_RESOURCE_INTERRUPT_MESSAGE;
    raw.descriptors[1].u.MessageInterrupt.Raw.MessageCount = 1;
    assert(select_resources() == STATUS_SUCCESS);
    raw.descriptors[1].u.MessageInterrupt.Raw.MessageCount = 2;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    raw.descriptors[1].u.MessageInterrupt.Raw.MessageCount = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    translated.descriptors[1].Flags = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); raw.descriptors[1].Type = translated.descriptors[1].Type = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); raw.descriptors[1].Type = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); raw.descriptors[2] = translated.descriptors[2] = raw.descriptors[1];
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);

    init(); raw.descriptors[3].Type = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); raw.descriptors[2] = raw.descriptors[3]; translated.descriptors[2] = translated.descriptors[3];
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.count--;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Length--;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Length = raw.descriptors[3].u.Memory.Length = 0x2000;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Length = raw.descriptors[3].u.Memory.Length = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Start.QuadPart = INT64_MAX - 0xfff;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Start.QuadPart = 0;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Start.QuadPart = -4096;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Start.QuadPart++;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].Flags = CM_RESOURCE_MEMORY_READ_ONLY;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    init(); translated.descriptors[3].u.Memory.Start.QuadPart = 0x100101000;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);

    init(); raw.descriptors[2] = (DESC){.Type=CmResourceTypeMemory, .u.Memory={{0xDEEAB000}, 0x1000}};
    translated.descriptors[2] = raw.descriptors[2];
    assert(select_resources() == STATUS_SUCCESS);
    raw.descriptors[2].u.Memory.Length = translated.descriptors[2].u.Memory.Length = 0x1001;
    assert(select_resources() == STATUS_DEVICE_CONFIGURATION_ERROR);
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="pj_kmdf_resources_") as temp:
    src = Path(temp) / "test.c"
    exe = Path(temp) / ("test.exe" if os.name == "nt" else "test")
    src.write_text(fixture + mapping + selector + cases)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")), str(src), "-o", str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)
print("PASS: PCI raw/translated BAR pairing, IRQ validation, and idempotent MMIO rollback")
