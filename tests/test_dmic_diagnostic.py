#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise diagnostic-only native NHLT reporting without accessing hardware."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
device = (root / "src/driver/Device.c").read_text()
body = device[device.index("static VOID SofLogDmicProfile(VOID)"):device.index("\n/* Verify the exact source fallback",device.index("static VOID SofLogDmicProfile(VOID)"))]
assert "SofNhltQueryPujjoFallback" not in body
assert "Wdf" not in body and "readl(" not in body and "writel(" not in body
prepare = device[device.index("SofEvtDevicePrepareHardware("):]
assert prepare.index("SofUnmapPreparedBars(ctx);") < prepare.index("SofLogDmicProfile();") < prepare.index("WdfInterruptCreate(")
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PujjoDmicProfile.h"
typedef int NTSTATUS;
typedef void VOID, *PVOID;
typedef uint32_t ULONG;
#define NT_SUCCESS(s) ((s)>=0)
#define PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u
#define RtlStringCbPrintfA snprintf
struct sof_ipc4_copier_data { ULONG words[16]; };
static int query_status, inspect_status, inspect_calls, frees, logs;
static ULONG blob_bytes;
static char last[256];
static int value;
static int SofNhltQueryAcpiTable(PVOID *table,ULONG *bytes) {
    assert(!*table && !*bytes);
    if(query_status) return query_status;
    *table=&value;*bytes=500;return 0;
}
static void SofNhltFreeAcpiTable(PVOID table) {
    assert(table==(query_status?NULL:&value));++frees;
}
static void SofLogBreadcrumb(const char *msg) {
    ++logs;
    if(strstr(msg,"mailbox-fit=")) snprintf(last,sizeof(last),"%s",msg);
}
static int SofNhltInspectPujjoDmic(PVOID table,ULONG bytes,pj_dmic_profile *p) {
    ++inspect_calls;assert(table==&value && bytes==500);memset(p,0,sizeof(*p));
    p->bytes=blob_bytes;p->pdm_clock_hz=3840000; return inspect_status;
}
''' + body + r'''
int main(void) {
    for (int q=-1;q<=0;++q) for(int i=-1;i<=0;++i) {
        query_status=q;inspect_status=i;inspect_calls=logs=frees=0;
        blob_bytes=1000;last[0]=0;
        SofLogDmicProfile();
        assert(frees==1 && inspect_calls==(q==0) && logs==(q==0?3:2));
        if(q==0 && i==0) assert(strstr(last,"mailbox-fit=1"));
        else assert(!last[0]);
    }
    query_status=inspect_status=0;blob_bytes=4096;
    SofLogDmicProfile();assert(strstr(last,"mailbox-fit=0"));
    puts("PASS: production passive diagnostic, absent/malformed/native NHLT, mailbox fit and buffer cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix="pj_dmic_diag_") as tmp:
    c=Path(tmp)/"dmic_diag.c";exe=Path(tmp)/("dmic_diag.exe" if os.name=="nt" else "dmic_diag")
    c.write_text(source)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),"-I"+str(root/"src/driver"),
                    str(c),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=15)
