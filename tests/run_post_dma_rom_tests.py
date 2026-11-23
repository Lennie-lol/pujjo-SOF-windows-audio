#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile and execute the actual post-DMA ROM gate using fake register samples.

Host-only. Does not access hardware, install drivers, arm a canary, or load SOF.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
loader = (root / "src/driver/Loader.c").read_text(encoding="utf-8")
marker = "static NTSTATUS SofCheckPostDmaRomState("
body = marker + loader.split(marker, 1)[1].split("\n}", 1)[0] + "\n}\n"

shim = r"""
#include <assert.h>
#include <stdint.h>
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
typedef int SOF_FW_BOOT_STATE;
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185)
#define STATUS_DEVICE_PROTOCOL_ERROR ((NTSTATUS)0xC0000186)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5)
#define SOF_FSR_HALTED_MASK 0x80000000u
#define FSR_STATE_FW_ENTERED 0x5u
#define FSR_TO_STATE_CODE(x) ((x) & 0x00ffffffu)
#define SOF_FW_BOOT_NOT_STARTED 0
#define SOF_FW_BOOT_IN_PROGRESS 1
#define SOF_FW_BOOT_FAILED 2
#define SOF_FW_BOOT_READY_RECEIVED 3
#define SOF_FW_BOOT_COMPLETE 4
"""
cases = r"""
int main(void) {
    /* No fault; module bits in FSR may be populated. */
    assert(SofCheckPostDmaRomState(0x00000005u, SOF_FW_BOOT_IN_PROGRESS) == STATUS_SUCCESS);
    assert(SofCheckPostDmaRomState(0x20000005u, SOF_FW_BOOT_READY_RECEIVED) == STATUS_SUCCESS);
    /* A once-good FW_ENTERED state can turn into a halt after DMA stops. */
    assert(SofCheckPostDmaRomState(0x80000005u, SOF_FW_BOOT_IN_PROGRESS) == STATUS_IO_DEVICE_ERROR);
    assert(SofCheckPostDmaRomState(0x80000001u, SOF_FW_BOOT_READY_RECEIVED) == STATUS_IO_DEVICE_ERROR);
    /* MMIO disappeared, without conflating this with a ROM timeout. */
    assert(SofCheckPostDmaRomState(UINT32_MAX, SOF_FW_BOOT_IN_PROGRESS) == STATUS_DEVICE_NOT_READY);
    /* ROM reset or regressed after DMA stop. */
    assert(SofCheckPostDmaRomState(0x00000001u, SOF_FW_BOOT_IN_PROGRESS) == STATUS_IO_TIMEOUT);
    assert(SofCheckPostDmaRomState(0x00000000u, SOF_FW_BOOT_IN_PROGRESS) == STATUS_IO_TIMEOUT);
    /* An invalid boot epoch can never be accepted with an apparently-good FSR. */
    assert(SofCheckPostDmaRomState(0x00000005u, SOF_FW_BOOT_FAILED) == STATUS_DEVICE_PROTOCOL_ERROR);
    assert(SofCheckPostDmaRomState(0x00000005u, SOF_FW_BOOT_NOT_STARTED) == STATUS_DEVICE_PROTOCOL_ERROR);
    assert(SofCheckPostDmaRomState(0x00000005u, SOF_FW_BOOT_COMPLETE) == STATUS_DEVICE_PROTOCOL_ERROR);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="pujjo_post_dma_rom_") as d:
    source = Path(d) / "check.c"
    exe = Path(d) / ("check.exe" if os.name == "nt" else "check")
    source.write_text(shim + body + cases, encoding="utf-8")
    compiler = os.environ.get("CC", "cc")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    str(source), "-o", str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)

print("PASS: compiled post-DMA ROM admission fails closed on HALTED, missing FSR, stale epoch and regression")
