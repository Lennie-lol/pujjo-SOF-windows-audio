#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute the production bounded PCI capability parser and inspect diagnostic isolation."""
from pathlib import Path
import os, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
device = (root / "src/driver/Device.c").read_text()
loader = (root / "src/driver/Loader.c").read_text()
begin = device.index("static NTSTATUS SofInspectPciCapabilities(")
parser = device[begin:device.index("\n}", begin)+2]
snapshot = loader.split("VOID SofLogColdBootReadiness(", 1)[1].split("\n}",1)[0]
for bad in ("writel(", "writew(", "WRITE_REGISTER_", "SofAcquireBootProcessing", "SofStartFirmwareBoot", "DeviceReset(", "WdfDmaEnablerCreate", "SofBeginDmaJournal"):
    assert bad not in snapshot + parser, bad
branch = device.split("A physical read-only pre-DMA milestone",1)[1].split("#else",1)[0]
assert branch.index("SofLogColdBootReadiness(ctx)") < branch.index("SofUnmapPreparedBars(ctx)")
assert "defined(PUJJO_DMIC_DMA_PREFLIGHT) || defined(PUJJO_DMIC_DETECTION_TEST)" in device
inf = (root / "packaging/diagnostics/pujjo_dsp_rom_preflight.inx").read_text()
assert "DriverVer=11/27/2026,0.0.12.0" in inf
for key in ("SafeBootPolicy", "BootSafetyToken", "ArmedTesting", "EnablePortCls", "EnableS0Idle", "EnableUsbOffload"):
    assert f"HKR,Parameters,{key},0x00010001,0" in inf
assert "RemappingSupported" not in inf and "AddInterface" not in inf
workflow = (root / ".github/workflows/compile-windows.yml").read_text()
assert "PUJJO_DSP_BOOT_DIAGNOSTIC /DPUJJO_DMIC_ROM_PREFLIGHT" in workflow
assert "Pujjo-v0.0.12.0-ROM-READINESS-UNSIGNED" in workflow
assert "-RomPreflight" in workflow
fixture = r"""
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef uint32_t ULONG;
typedef unsigned char UCHAR;
typedef int NTSTATUS;
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_DEVICE_CONFIGURATION_ERROR -2
#define STATUS_DEVICE_NOT_READY -3
"""
cases = r"""
int main(void) {
 UCHAR c[256]={0};
 ULONG ex=7, flr=7, pending=7, pm=7;
 assert(!SofInspectPciCapabilities(c,sizeof(c),&ex,&flr,&pending,&pm));
 assert(!ex&&!flr&&!pending&&pm==MAXULONG);
 c[6]=0x10;c[0x34]=0x40;c[0x40]=0x01;c[0x41]=0x48;c[0x42]=3;c[0x44]=3;
 c[0x48]=0x10;c[0x4a]=2;c[0x4f]=0x10;c[0x52]=0x20;
 assert(!SofInspectPciCapabilities(c,sizeof(c),&ex,&flr,&pending,&pm));
 assert(ex==1&&flr==1&&pending==1&&pm==3);
 /* Every truncation before PCIe Device Status is rejected. */
 for(ULONG n=0;n<0x54;n++) {
   ex=flr=pending=pm=7;
   assert(SofInspectPciCapabilities(c,n,&ex,&flr,&pending,&pm)<0);
   assert(ex==7&&flr==7&&pending==7&&pm==7);
 }
 /* A capability pointer may not visit itself, overlap header, or cycle. */
 c[0x49]=0x40; assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0x48; assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0x4c; assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0x41; assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0x10; assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0xfc;c[0xfc]=0x10;assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x49]=0;c[0x48]=0xff;assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
 c[0x48]=0x10;c[0x4f]=0;c[0x52]=0;
 assert(!SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm));
 assert(ex==1&&!flr&&!pending&&pm==3);
 c[0x34]=0;assert(SofInspectPciCapabilities(c,256,&ex,&flr,&pending,&pm)<0);
}
"""
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/"pci.c"; p.write_text(fixture+parser+cases)
    exe=p.with_suffix("")
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(p),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
print("PASS: bounded PCI capability evidence, no reset/domain/ROM execution, distinct guarded INF")
