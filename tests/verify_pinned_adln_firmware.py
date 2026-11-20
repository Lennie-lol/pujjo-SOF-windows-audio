#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check a pinned upstream firmware artifact against production metadata code.

No firmware is executed, redistributed in CI artifacts, or authorized for boot.
A passing hash and manifest check is NOT ROM authentication or hardware proof.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REVISION = "fcd5ae85ead7ecd67ff199335b61fac825843ca0"
BLOB = "47bb69dd2e22435e6da37ccee17103e0f6452638"
SHA256 = "bc83cffa1bb5a4a8da24bbc7bb7040bd744f1e0c63e3d16151d1769ac8ae8214"
LENGTH = 707036

def check(path, report_path):
    if path.stat().st_size != LENGTH:
        raise ValueError("unexpected pinned firmware length")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("SHA-256 mismatch")
    if hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() != BLOB:
        raise ValueError("Git object mismatch")
    loader = (ROOT / "src/driver/Loader.c").read_text()
    digest_body = re.search(r"static const UCHAR expected\[32\]\s*=\s*\{([^}]+)\}", loader)
    if not digest_body or bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", digest_body[1])).hex() != SHA256:
        raise ValueError("kernel firmware pin differs from tested artifact")
    if f"ImageBytes != {LENGTH}u" not in loader:
        raise ValueError("kernel firmware length differs from tested artifact")
    roles = []
    for name in ("copierUuid", "gainUuid"):
        m = re.search(r"static const PJ_BYTE " + name + r"\[16\]\s*=\s*\{([^}]+)\}", loader)
        if not m:
            raise ValueError("missing production UUID: " + name)
        values = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", m[1])]
        if len(values) != 16:
            raise ValueError("invalid production UUID")
        roles.append("{" + ",".join(str(x) for x in values) + "}")
    fixture = r'''
#include "PujjoFirmwareCatalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static const PJ_BYTE uuids[2][16] = {UUIDS};
int main(int argc, char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");
    assert(f);
    assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);
    assert(n==707036L);
    rewind(f);
    PJ_BYTE *image=malloc((size_t)n);
    assert(image && fread(image,1,(size_t)n,f)==(size_t)n);
    fclose(f);
    pj_firmware_profile p[2];
    for(unsigned i=0;i<2;++i) {
        assert(pj_firmware_load_profile(image,(size_t)n,uuids[i],&p[i])==PJ_CATALOG_OK);
        assert(p[i].module.instance_max_count >= (i==0?4u:2u));
        printf("role=%s module=%u instances=%u records=%u affinity=%u type=%u\n",
            i==0?"copier":"gain",p[i].module.module_id,
            (unsigned)p[i].module.instance_max_count,p[i].count,
            p[i].module.affinity_mask,p[i].module.type);
    }
    assert(p[0].module.module_id!=p[1].module.module_id);
    int dmic_supported=0;
    for(unsigned j=0;j<3;++j) {
        PJ_DWORD chunk=j==2?768u:(j?384u:192u), cp=0,gp=0;
        pj_firmware_config c={0},g={0};
        pj_catalog_status a=pj_firmware_plan_ll(&p[0],chunk,chunk,&c,&cp);
        pj_catalog_status b=pj_firmware_plan_ll(&p[1],chunk,chunk,&g,&gp);
        int supported=a==PJ_CATALOG_OK && b==PJ_CATALOG_OK && 2u*cp+gp<=20u;
        printf("chunk=%u supported=%d copier_status=%d gain_status=%d copier_pages=%u gain_pages=%u copier_cpc=%u gain_cpc=%u\n",
            chunk,supported,(int)a,(int)b,cp,gp,c.cpc,g.cpc);
        if(j<2 && supported)dmic_supported=1;
        if(j==1)assert(supported); /* all analog routes use stereo 32-bit at 48kHz */
    }
    assert(dmic_supported);
    free(image);
    return 0;
}
'''.replace("UUIDS", ",".join(roles))
    with tempfile.TemporaryDirectory(prefix="pj_real_fw_") as tmp:
        src = Path(tmp) / "catalog.c"
        exe = Path(tmp) / ("catalog.exe" if os.name == "nt" else "catalog")
        src.write_text(fixture)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        *shlex.split(os.environ.get("CFLAGS", "")),
                        "-I" + str(ROOT / "src/driver"),
                        str(ROOT / "src/driver/PujjoFirmwareCatalog.c"), str(src), "-o", str(exe)],
                       check=True, timeout=60)
        run = subprocess.run([str(exe), str(path.resolve())], text=True, capture_output=True, timeout=20)
        print(run.stdout, end="")
        if run.returncode:
            print(run.stderr, end="")
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps({
            "revision": REVISION, "git_blob": BLOB, "bytes": LENGTH, "sha256": SHA256,
            "catalog_test_exit": run.returncode, "catalog_test_output": run.stdout,
            "catalog_test_error": run.stderr, "hardware_verified": False,
            "rom_authenticated": False, "activation_authorized": False,
        }, indent=2) + "\n")
        run.check_returncode()
    print("PASS: pinned artifact, production module lookup and candidate audio budgets; no boot performed")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    check(args.firmware, args.report)
