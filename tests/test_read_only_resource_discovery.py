#!/usr/bin/env python3
"""Read-only PnP discovery regression: not a physical test."""
from pathlib import Path
src=(Path(__file__).resolve().parents[1]/"src/driver/Device.c").read_text()
caps=src.split("VOID SofDiscoverHdaCaps(",1)[1].split("\n/**",1)[0]
prep=src.split("SofEvtDevicePrepareHardware(",1)[1].split("\n/**",1)[0]
assert "writel(" not in caps and "WRITE_REGISTER_" not in caps
assert "busInterface.SetBusData(" not in prep
assert "PCI Command updated" not in prep
assert "writel(" not in prep and "WRITE_REGISTER_" not in prep
assert "SofHardwareDevelopmentAllowed()" in prep
assert prep.index("!SofHardwareDevelopmentAllowed()") < prep.index("WdfFdoQueryForInterface")
assert "SofDiscoverHdaCaps(ctx)" in prep
assert "SofIpcInitialize(ctx)" in prep
assert "no CRST writes" in prep
assert "ctx->Bar0Length < SOF_HDA_WAKESTS + sizeof(USHORT)" in prep
print("PASS: PCI/HDA resource discovery is read-only and checks codec status bounds")

# Diagnostic reporting must not open an SPB or GPIO target in default builds.
peripheral=(Path(__file__).resolve().parents[1]/"src/driver/peripheral/PujjoPeripheralKmdf.c").read_text()
match=peripheral.split("static BOOLEAN PjHardwareIdMatches(",1)[1].split("\n}",1)[0]
assert "ids[total - 1] != 0 || ids[total - 2] != 0" in match
assert "matched = TRUE;" in match and "return TRUE;" not in match
assert match.index("if (ids[pos++] != 0) return FALSE;") < match.index("return matched;")
prepare=peripheral.split("NTSTATUS PjEvtPrepareHardware(",1)[1].split("\n}",1)[0]
assert "Resource policy rejected:" in prepare and "Resource policy accepted:" in prepare
assert "connection class=" in prepare
assert prepare.index("if (result != PJ_POLICY_OK)") < prepare.index("ctx->Evidence = summary;") < prepare.index("Pj5682sSpbOpen(")
assert prepare.index("#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)") < prepare.index("Pj5682sSpbOpen(")
assert "Pj1019" not in prepare
print("PASS: peripheral diagnostics report translated resources without default activation")

transport=(Path(__file__).resolve().parents[1]/"src/driver/peripheral/PujjoRt5682sSpbWindows.c").read_text()
assert "WdfIoTargetSendWriteSynchronously" not in transport
assert "bus->write_only=NULL;" in transport
read=transport.split("static int Pj5682sSpbRead(",1)[1].split("\n}",1)[0]
for register in ("PJ5682S_DEVICE_ID_REGISTER","PJ5682S_VENDOR_ID_REGISTER","PJ5682S_VERSION_REGISTER"):
    assert read.index(register) < read.index("WdfIoTargetSendIoctlSynchronously")
assert "tx[0] != 0" in read and "txLen!=2" in read
# Both transport status and exact byte count must be checked before publishing.
assert "SpbTransferDirectionFromDevice, 0, observed, (ULONG)sizeof(observed)" in read
guard = "if (!NT_SUCCESS(status) || transferred != txLen + rxLen)"
assert read.index("WdfIoTargetSendIoctlSynchronously") < read.index(guard)
failure = read.split(guard, 1)[1].split("}", 1)[0]
assert "return -1;" in failure and "WdfWaitLockRelease(state->Lock);" in failure
assert read.index(guard) < read.index("rx[0] = observed[0];") < read.index("rx[1] = observed[1];")
assert read.count("rx[0] =") == 1 and read.count("rx[1] =") == 1
print("PASS: Windows SPB transport restricts physical transactions to identity reads")
