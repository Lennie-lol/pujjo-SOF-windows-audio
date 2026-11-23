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
assert "if (!g_CanaryArmed || g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT)" in prepare
assert prepare.index("g_SafeBootPolicy != SOF_SAFE_BOOT_SINGLE_SHOT") < prepare.index(
    "WdfFdoQueryForInterface"
)
d0 = device.split("SofEvtDeviceD0Entry(", 1)[1]
assert "g_CanaryArmed = FALSE;" in d0
assert d0.index("g_CanaryArmed = FALSE;") < d0.index("SofStartFirmwareBoot(ctx, NULL);")

for name in ("SafeBootPolicy", "BootSafetyToken", "ArmedTesting", "EnablePortCls"):
    assert f"HKR,Parameters,{name},0x00010001,0" in inf
assert "DriverVer=11/23/2026,0.0.2.0" in inf
assert "'tests/test_forced_safe_kmdf_stub.py'" in suite
print("PASS: forced-safe KMDF-only stub precedes PnP callbacks/DMA; one-shot DSP guard preserved")
