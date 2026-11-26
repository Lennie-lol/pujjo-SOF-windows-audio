#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Detection-only endpoint: execute production metadata/denial callbacks on host.

This is not a Windows PnP or PortCls integration test.
"""
from pathlib import Path
import os
import shlex
import struct
import subprocess
import tempfile
import uuid

root = Path(__file__).resolve().parents[1]
driver = root / "src/driver"
pdo = (driver / "PujjoDmicDetection.c").read_text()
port = (driver / "PortClsAdapter.cpp").read_text()
device = (driver / "Device.c").read_text()
entry = (driver / "Driver.c").read_text()
header = (driver / "PujjoAudioBus.h").read_text()
inf = (root / "packaging/diagnostics/pujjo_dmic_detection_child.inx").read_text()
parent = (root / "packaging/diagnostics/pujjo_dsp_dmic_detection.inx").read_text()
workflow = (root / ".github/workflows/compile-windows.yml").read_text()
assert "defined(PUJJO_ENABLE_EXPERIMENTAL_AUDIO)" in pdo.split("#error")[0]
assert "defined(PUJJO_ENABLE_EXPERIMENTAL_ANALOG)" in pdo.split("#error")[0]
assert "PUJJO_DSP_BOOT_DIAGNOSTIC" in pdo.split("#error")[0]
for forbidden in ("MmMapIoSpace(", "WRITE_REGISTER_", "READ_REGISTER_", "writel(", "WdfInterruptCreate(",
                  "WdfDmaEnablerCreate(", "SofStartFirmwareBoot(", "SofWaveRt", "SofIpc", "->ReferenceCount"):
    assert forbidden not in pdo, forbidden
probe = device.split("A physical read-only pre-DMA milestone", 1)[1].split("#else",1)[0]
assert probe.index("SofUnmapPreparedBars(ctx)") < probe.index("SofPublishDmicDetectionChild(ctx)")
assert "PUJJO_DMIC_DETECTION_TEST" in probe and "goto PreparationFailed" in probe
assert "WdfFdoLockStaticChildListForIteration" in pdo
assert pdo.index("InterlockedExchange(&PujjoDetectionContext(child)->Online, 0)") < pdo.index("WdfPdoMarkMissing(child)")
assert "WdfObjectReferenceWithTag((WDFDEVICE)Context" in pdo
assert "WdfObjectDereferenceWithTag((WDFDEVICE)Context" in pdo
assert "bus.Header.Context = child;" in pdo
assert "bus.Header.Context = Ctx;" not in pdo
assert "WdfDeviceAddQueryInterface(child" in pdo and "WdfFdoAddStaticChild" in pdo
assert pdo.index("WdfDeviceAddQueryInterface(child") < pdo.index("WdfFdoAddStaticChild")
assert "SofWithdrawDmicDetectionChild(Device)" in device
assert "pnpPowerCallbacks.EvtDeviceSurpriseRemoval = SofEvtDetectionSurpriseRemoval" in entry
assert "GUID_PUJJO_DMIC_DETECTION_BUS" in pdo and "GUID_PUJJO_DMIC_DETECTION_BUS" in port
assert "GUID_PUJJO_AUDIO_BUS" not in pdo
assert r'PUJJO\\AudioDmicDetectionTest' in header and r'PUJJO\AudioDmicDetectionTest' in inf
new_stream = port.split("STDMETHODIMP NewStream(", 1)[1].split("\n    }", 1)[0]
assert "*Out = nullptr;" in new_stream
deny = new_stream.split("#ifdef PUJJO_DMIC_DETECTION_TEST_ADAPTER",1)[1].split("#else",1)[0]
assert "return STATUS_DEVICE_NOT_READY;" in deny and "new (" not in deny
for value in ("SafeBootPolicy", "BootSafetyToken", "ArmedTesting", "EnablePortCls", "EnableS0Idle", "EnableUsbOffload"):
    assert f"HKR,Parameters,{value},0x00010001,0" in parent
assert "0.0.8.1" in parent and "0.0.8.1" in inf
assert "AddService=pujjo_audio_detection" in inf and "CatalogFile=pujjo_audio_detection.cat" in inf
assert "Detection test - recording disabled" in inf
assert r'HKR,EP\0,%PKEY_AudioDevice_NeverSetAsDefaultEndpoint%,0x00010001,0x00000307' in inf
assert r'HKR,MediaCategories\%DetectionPinName%,Name,,%Device%' in inf
metadata = next(line for line in inf.splitlines() if "HKR,EP" in line and "OEMFormat" in line)
blob = bytes.fromhex(metadata.split("0x00000001,",1)[1].replace(",",""))
assert len(blob) == 48
assert struct.unpack("<IIHHIIHHHHI", blob[:32]) == (65,40,0xfffe,4,48000,768000,16,32,22,32,0)
assert uuid.UUID(bytes_le=blob[32:]) == uuid.UUID("00000001-0000-0010-8000-00aa00389b71")
assert "guarded-dmic-detection-parent/bin" in workflow and "guarded-dmic-detection-child/bin" in workflow

callbacks = pdo[pdo.index("static NTSTATUS DetectionFormat"):pdo.index("static EVT_WDF_OBJECT_CONTEXT_CLEANUP")]
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef void *PVOID;
typedef void *WDFDEVICE;
typedef void *PKEVENT;
typedef unsigned ULONG;
typedef ULONG *PULONG;
typedef int NTSTATUS;
typedef bool BOOLEAN;
typedef void VOID;
typedef struct { unsigned Rate, Channels, ContainerBits, ValidBits, Role; } PUJJO_AUDIO_FORMAT;
typedef struct { void *Stream, *Mdl; unsigned Bytes, CacheType; } PUJJO_AUDIO_BUFFER;
typedef struct { long Online; PUJJO_AUDIO_FORMAT Format; } PUJJO_DETECTION_CONTEXT, *PPUJJO_DETECTION_CONTEXT;
typedef struct { int Source; } pj_dmic_profile;
#define RtlZeroMemory(p,n) memset(p,0,n)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DEVICE_NOT_READY (-2)
#define STATUS_NOT_FOUND (-3)
#define STATUS_OBJECT_NAME_NOT_FOUND (-4)
#define NT_SUCCESS(s) ((s)>=0)
static long InterlockedCompareExchange(long *p,long value,long expected) {
    long old=*p; if(old==expected) *p=value; return old;
}
static PPUJJO_DETECTION_CONTEXT PujjoDetectionContext(WDFDEVICE child) { return child; }
static int queryStatus, inspectStatus, embeddedStatus, nativeCalls, embeddedCalls, freeCalls;
static NTSTATUS SofNhltQueryAcpiTable(PVOID *table, ULONG *bytes) {
    *table= queryStatus==0 ? (void *)1 : NULL; *bytes=100; return queryStatus;
}
static NTSTATUS SofNhltInspectPujjoDmic(PVOID table, ULONG bytes,pj_dmic_profile *profile) {
    assert(table && bytes==100); nativeCalls++; profile->Source=1; return inspectStatus;
}
static NTSTATUS SofNhltInspectEmbeddedDmic(pj_dmic_profile *profile) {
    embeddedCalls++; profile->Source=2; return embeddedStatus;
}
static void SofNhltFreeAcpiTable(PVOID table) { (void)table; freeCalls++; }
'''
cases = r'''
int main(void) {
    PUJJO_DETECTION_CONTEXT ctx={1,{48000,4,32,32,0}};
    PUJJO_AUDIO_FORMAT format={0};
    PUJJO_AUDIO_BUFFER buf={(void *)1,(void *)2,1024,3};
    unsigned position=100;
    assert(DetectionFormat(&ctx,&format)==STATUS_SUCCESS);
    assert(format.Rate==48000 && format.Channels==4 && format.ValidBits==32 && format.Role==0);
    for(int online=0;online<=1;online++) {
        ctx.Online=online;
        assert(DetectionAllocate(&ctx,1024,&buf)==STATUS_DEVICE_NOT_READY);
        assert(!buf.Stream && !buf.Mdl && !buf.Bytes && !buf.CacheType);
        assert(DetectionAllocate(&ctx,0,NULL)==STATUS_DEVICE_NOT_READY);
        DetectionFree(&ctx,NULL);
        for(unsigned state=0;state<4;state++)
            assert(DetectionSetState(&ctx,NULL,state)==STATUS_DEVICE_NOT_READY);
        assert(DetectionPosition(&ctx,NULL,&position)==STATUS_DEVICE_NOT_READY && !position);
        assert(DetectionNotification(NULL,NULL,true)==STATUS_DEVICE_NOT_READY);
        assert(DetectionNotification(NULL,NULL,false)==STATUS_DEVICE_NOT_READY);
    }
    ctx.Online=0;
    assert(DetectionFormat(&ctx,&format)==STATUS_DEVICE_NOT_READY);
    assert(!format.Rate && !format.Channels && !format.ContainerBits && !format.ValidBits);
    assert(DetectionFormat(NULL,&format)==STATUS_INVALID_PARAMETER);
    assert(DetectionFormat(&ctx,NULL)==STATUS_INVALID_PARAMETER);
    pj_dmic_profile p={0};
    assert(DetectionSelectProfile(&p)==0 && p.Source==1 && nativeCalls==1 && !embeddedCalls);
    inspectStatus=-7;
    assert(DetectionSelectProfile(&p)==-7 && nativeCalls==2 && !embeddedCalls);
    queryStatus=STATUS_NOT_FOUND;
    assert(DetectionSelectProfile(&p)==0 && p.Source==2 && embeddedCalls==1);
    queryStatus=STATUS_OBJECT_NAME_NOT_FOUND;
    embeddedStatus=-8;
    assert(DetectionSelectProfile(&p)==-8 && embeddedCalls==2);
    queryStatus=-9;
    assert(DetectionSelectProfile(&p)==-9 && embeddedCalls==2 && nativeCalls==2 && freeCalls==5);
}
'''
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "detector"
    source = Path(directory) / "detector.c"
    source.write_text(mock + callbacks + cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    *shlex.split(os.environ.get("CFLAGS","")),str(source),"-o",str(exe)],check=True,timeout=40)
    subprocess.run([str(exe)],check=True,timeout=20)
print("PASS: detection-only PDO metadata, native/fallback selection, all capture callbacks deny, distinct identity and endpoint INF")
