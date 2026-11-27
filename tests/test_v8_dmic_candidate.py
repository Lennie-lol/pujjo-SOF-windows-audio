#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The v8 candidate uses real DMIC route data but cannot load or execute."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/"scripts"))
from generate_pujjo_dmic_header import render

driver = (root/"src/driver/Driver.c").read_text()
device = (root/"src/driver/Device.c").read_text()
loader = (root/"src/driver/Loader.c").read_text()
nhlt = (root/"src/driver/Nhlt.c").read_text()
header = (root/"src/driver/PujjoDmicNhltBytes.h").read_text()
workflow = (root/".github/workflows/compile-windows.yml").read_text()
candidate = bytes(int(v,16) for v in re.findall(r"0x([0-9a-f]{2})", header))
assert render(candidate)==header
for offset in (0, 9, 36, 310, len(candidate)-1):
    modified=bytearray(candidate)
    modified[offset]^=1
    try:
        render(modified)
    except ValueError:
        pass
    else:
        raise AssertionError("changed candidate accepted")
entry = driver.split("DriverEntry(\n",1)[1].split("#else",1)[0]
assert "PUJJO_DMIC_ENUMERATION_CANDIDATE" in entry
assert "return STATUS_NOT_SUPPORTED;" in entry
for forbidden in ("WdfDriverCreate", "SofCheckAndConsumeCanaryFile", "ZwOpenKey"):
    assert forbidden not in entry
boot = loader.split("SofStartFirmwareBoot(", 1)[1].split(
    "#if defined(PUJJO_DMIC_ENUMERATION_CANDIDATE) && !defined(PUJJO_DMIC_ROM_BOOT_CANDIDATE)", 1)[1].split("#else", 1)[0]
assert "return STATUS_NOT_SUPPORTED;" in boot
assert "SofNhltQueryPujjoDmicFallback(nhltStatus," in device
assert "SofNhltInspectPujjoDmic(nhltBuffer, nhltSize" in device
assert "SofNhltInspectEmbeddedDmic(&profile)" in nhlt
assert "dmic-enumeration-compile-only" in workflow
assert "guarded-dmic-enumeration-compile-only/bin" not in workflow
assert "generate_pujjo_dmic_header.py" in (root/"scripts/run_alsa_ssp_reference.sh").read_text()

# Query the actual reset-capability reporting function with a mock bus.
helper = device[device.index("static VOID SofLogResetCapability("):
                device.index("\n#endif",device.index("static VOID SofLogResetCapability("))]
assert "reset.DeviceReset(" not in helper
assert "SofLogResetCapability(Device)" in device
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void VOID,*PVOID,*PINTERFACE,*WDFDEVICE;
typedef int NTSTATUS;
typedef struct {
    unsigned Size,Version;
    void *Context;
    void (*InterfaceReference)(void *);
    void (*InterfaceDereference)(void *);
    int (*DeviceReset)(void *);
    unsigned SupportedResetTypes;
} DEVICE_RESET_INTERFACE_STANDARD;
#define NT_SUCCESS(s) ((s)>=0)
#define RtlStringCbPrintfA snprintf
static int GUID_DEVICE_RESET_INTERFACE_STANDARD;
static int queries,derefs,result,context;
static char logmsg[160];
static int fake_reset(void *c) { (void)c; assert(0 && "probe called reset"); return -1; }
static void deref(void *c) { assert(c==&context); ++derefs; }
static int WdfFdoQueryForInterface(void *d,void *g,void *p,size_t size,int version,void *data) {
    assert(d==&context && g==&GUID_DEVICE_RESET_INTERFACE_STANDARD && !data);
    assert(size==sizeof(DEVICE_RESET_INTERFACE_STANDARD) && version==1);
    ++queries;
    if(result<0) return result;
    DEVICE_RESET_INTERFACE_STANDARD *r=p;
    assert(!r->DeviceReset && !r->SupportedResetTypes);
    r->Context=&context;
    r->InterfaceDereference=deref;
    r->DeviceReset=fake_reset;
    r->SupportedResetTypes=3;
    return 0;
}
static void SofLogBreadcrumb(const char *s) { snprintf(logmsg,sizeof(logmsg),"%s",s); }
'''
cases = r'''
int main(void) {
    result=-5; SofLogResetCapability(&context);
    assert(queries==1 && derefs==0 && strstr(logmsg,"unavailable"));
    result=0; SofLogResetCapability(&context);
    assert(queries==2 && derefs==1 && strstr(logmsg,"types=0x00000003 handler=1; not invoked"));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/"test.c";exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
    src.write_text(fixture+helper+cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS: pinned DMIC-only candidate; no installation path; read-only reset query and reference release")
