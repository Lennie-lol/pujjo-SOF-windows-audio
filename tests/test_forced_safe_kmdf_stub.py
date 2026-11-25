#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Offline structural guard for the forced-safe, KMDF-only DSP diagnostic.

A static source check does NOT qualify the driver for physical hardware.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
driver = (ROOT / "src/driver/Driver.c").read_text(encoding="utf-8")
device = (ROOT / "src/driver/Device.c").read_text(encoding="utf-8")
inf = (ROOT / "packaging/diagnostics/pujjo_dsp_boot_only.inx").read_text(encoding="utf-8")
suite = (ROOT / "tests/run_offline_suite.py").read_text(encoding="utf-8")

add = driver.split("SofEvtDeviceAdd(", 1)[1].split("SofEvtDriverUnload(", 1)[0]
safe = re.search(
    r"#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC.*?"
    r"(?P<block>if \(g_SafeBootPolicy == SOF_SAFE_BOOT_FORCED_SAFE && !g_CanaryArmed\)"
    r" \{.*?\n    \})\s*#endif",
    add,
    re.S,
)
assert safe is not None, "missing boot-only, disarmed forced-safe branch"
body = safe.group("block")
assert "WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device)" in body
assert "return status;" in body
assert body.index("WdfDeviceCreate(") < body.index("return status;")
assert add.index("#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC") < add.index(
    "WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks)"
), "The PnP-only stub must precede driver PnP callback registration"
assert add.index("return status;", add.index("#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC")) < add.index(
    "WdfDmaEnablerCreate("
), "Forced-safe mode must return before DMA enabler construction"
for prohibited in (
    "WdfDmaEnablerCreate", "WdfInterruptCreate", "WdfTimerCreate",
    "WdfDeviceAssignS0IdleSettings", "WdfDeviceInitSetPnpPowerEventCallbacks",
    "MmMapIoSpace", "SofStartFirmwareBoot", "SofEvtDevicePrepareHardware",
    "READ_REGISTER_", "WRITE_REGISTER_",
):
    assert prohibited not in body, f"Hardware or callbacks in forced-safe body: {prohibited}"

# If the registry key/value is missing or unexpected, only the diagnostic
# build should default to forced-safe; guarded DSP startup must still require
# the existing one-shot authorization at both PrepareHardware and D0Entry.
assert "ULONG g_SafeBootPolicy = SOF_SAFE_BOOT_FORCED_SAFE;" in driver
assert 'SofReadRegistryDword(paramsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_FORCED_SAFE)' in driver
assert "if (g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT)" in driver
prepare = device.split("SofEvtDevicePrepareHardware(", 1)[1]
assert "#ifndef PUJJO_DSP_BOOT_DIAGNOSTIC" in add.split("SofSdcaFilterDeviceAdd")[0].rsplit("#endif", 1)[-1]
assert prepare.index("g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT") < prepare.index(
    "WdfFdoQueryForInterface"
)
d0 = device.split("SofEvtDeviceD0Entry(", 1)[1]
assert "|| defined(PUJJO_DSP_BOOT_DIAGNOSTIC)" in d0.split("#else", 1)[0]
assert "return STATUS_NOT_SUPPORTED;" in d0.split("#else", 1)[0]

for name in ("SafeBootPolicy", "BootSafetyToken", "ArmedTesting", "EnablePortCls"):
    assert f"HKR,Parameters,{name},0x00010001,0" in inf
assert re.findall(r"(?im)^\s*DriverVer\s*=\s*([^\r\n]+)", inf) == ["11/25/2026,0.0.5.0"]
assert "DriverVer=11/23/2026,0.0.2.0" not in inf
stage = (ROOT / "scripts/Stage-PujjoDspBootDiagnostic.ps1").read_text(encoding="utf-8")
verify = (ROOT / "scripts/Verify-PujjoDspBootDiagnostic.ps1").read_text(encoding="utf-8")
assert "Package version 0.0.5.0;" in stage
assert "Package version 0.0.5.0;" in verify
assert "Wrong diagnostic package version; expected 0.0.5.0" in verify
assert "'tests/test_forced_safe_kmdf_stub.py'" in suite
entry = driver.split("DriverEntry(", 1)[1].split("WDF_DRIVER_CONFIG_INIT", 1)[0]
assert entry.index('SofWriteRegistryDword(paramsKey, L"BootSafetyToken", 0)') < entry.index('SofWriteRegistryDword(paramsKey, L"SafeBootPolicy", SOF_SAFE_BOOT_FORCED_SAFE)') < entry.index("NT_SUCCESS(ZwFlushKey(paramsKey))") < entry.index("g_CanaryArmed = TRUE;")
assert "if (!g_CanaryArmed) {\n        g_SafeBootPolicy = SOF_SAFE_BOOT_FORCED_SAFE;" in entry
assert "#ifndef PUJJO_DSP_BOOT_DIAGNOSTIC\n    pnpPowerCallbacks.EvtDeviceSurpriseRemoval" in add
probe_add = add.split("Read-only one-shot resource discovery only", 1)[1].split("#else", 1)[0]
assert "return STATUS_SUCCESS;" in probe_add and "WdfDmaEnablerCreate(" not in probe_add
probe = prepare.split("A physical read-only pre-DMA milestone", 1)[1].split("#else", 1)[0]
for forbidden in ("writel(", "writew(", "WRITE_REGISTER_", "SofIpcInitialize(", "WdfInterruptCreate(", "SofStartFirmwareBoot(", "SofAllocateDmaBuffer("):
    assert forbidden not in probe
assert "SofDiscoverHdaCaps(ctx)" in probe and "SofUnmapPreparedBars(ctx)" in probe
assert "InterlockedCompareExchange(&g_DiagnosticPreparePermit, 0, 1) != 1" in prepare.split("WdfFdoQueryForInterface", 1)[0]
loader = (ROOT / "src/driver/Loader.c").read_text(encoding="utf-8")
boot = loader.split("SofStartFirmwareBoot(", 1)[1]
assert "#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT) || defined(PUJJO_DSP_BOOT_DIAGNOSTIC)" in boot
assert "return STATUS_NOT_SUPPORTED;" in boot.split("#else", 1)[0]
print("PASS: persistent one-shot disarm, read-only resource probe and DMA activation hold")
