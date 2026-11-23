#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Read-only source contracts for experimental Pujjo ROM HALTED handling.

Not an on-device ROM test: never maps PCI BARs, DMA, or arms the canary.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hw = (root / "src/driver/Hw.h").read_text(encoding="utf-8")
loader = (root / "src/driver/Loader.c").read_text(encoding="utf-8")
driver = (root / "src/driver/Driver.c").read_text(encoding="utf-8")
assert "#define SOF_FSR_HALTED_MASK               0x80000000u" in hw
for raw, halted in ((0, False), (1, False), (5, False),
                    (0x80000000, True), (0x80000001, True),
                    (0x80000005, True), (0xffffffff, True)):
    assert bool(raw & 0x80000000) == halted

boot = loader.split("NTSTATUS\nSofStartFirmwareBoot(", 1)[1]
file_read = loader.split("NTSTATUS\nSofLoadFirmwareFile(", 1)[1].split("VOID\nSofFreeFirmwareBuffer(", 1)[0]
length_check = file_read.index("fileInfo.EndOfFile.QuadPart != SOF_ADLN_IPC4_IMAGE_BYTES")
assert length_check < file_read.index("return STATUS_FILE_INVALID;") < file_read.index("ExAllocatePool2(")
assert "ioStatus.Information != fileSize" in file_read
assert "ImageBytes != SOF_ADLN_IPC4_IMAGE_BYTES" in loader
initial = boot.split("/* Wait for ROM status FSR_STATE_INIT_DONE", 1)[1].split(
    "/* ROM ACK belongs to the loader", 1)[0]
no_ack = boot.split("if (timeout == 2000u)", 1)[1].split("/* Acknowledge ROM */", 1)[0]
assert "PURGE ACK timeout" in no_ack and "RomErrorReg" in no_ack
assert no_ack.index("statusReg == MAXULONG") < no_ack.index("statusReg & SOF_FSR_HALTED_MASK")
assert "STATUS_IO_DEVICE_ERROR : STATUS_IO_TIMEOUT" in no_ack and "goto boot_failed;" in no_ack
assert "INIT_DONE timeout" in initial
assert initial.index("statusReg == 0xFFFFFFFFu") < initial.index(
    "statusReg & SOF_FSR_HALTED_MASK") < initial.index(
    "FSR_TO_STATE_CODE(statusReg) == FSR_STATE_INIT_DONE")
assert "ROM halted before INIT_DONE" in initial
halt_init = initial.split("if (statusReg & SOF_FSR_HALTED_MASK)", 1)[1].split(
    "if (FSR_TO_STATE_CODE(statusReg)", 1)[0]
assert halt_init.index("status = STATUS_IO_DEVICE_ERROR;") < halt_init.index("goto boot_failed;")
assert "RomErrorReg" in initial

transfer = boot.split("timeout = 4000; /* 4 seconds */", 1)[1].split(
    "\nstop_dma:", 1)[0]
assert transfer.index("statusReg & SOF_FSR_HALTED_MASK") < transfer.index(
    "bootState == SOF_FW_BOOT_FAILED") < transfer.index(
    "FSR_TO_STATE_CODE(statusReg) == FSR_STATE_FW_ENTERED")
halt_dma = transfer.split("if (statusReg != 0xFFFFFFFFu && (statusReg & SOF_FSR_HALTED_MASK))", 1)[1].split(
    "if (bootState == SOF_FW_BOOT_FAILED", 1)[0]
assert halt_dma.index("ROM halted during firmware DMA") < halt_dma.index(
    "status = STATUS_IO_DEVICE_ERROR;") < halt_dma.index("goto stop_dma;")
assert "RomErrorReg" in transfer
assert "SOF_FW_BOOT_READY_RECEIVED" not in transfer

# Require a fresh BAR4 ROM read after both DMA channels have stopped and
# before a previously-seen FW_READY can be accepted.
post_dma = boot.split("status = SofStopIccmaxDma(Ctx, &iccmax);", 1)[1].split(
    "KeWaitForSingleObject", 1)[0]
assert post_dma.index("statusReg = SofDspRead32(bar4, Ctx->HwDesc->RomStatusReg);") < (
    post_dma.index("SofCheckPostDmaRomState(statusReg, bootState)"))
admission = post_dma.split("status = SofCheckPostDmaRomState(statusReg, bootState);", 1)[1]
assert admission.index("if (!NT_SUCCESS(status))") < admission.index("goto boot_failed;")
assert "Post-DMA ROM rejected" in post_dma
assert "SofQueryBootedFirmwareVersion" not in post_dma

# The known-good forced-safe KMDF entry remains the first PnP creation path.
add = driver.split("SofEvtDeviceAdd(", 1)[1]
stub = add.split("#ifdef PUJJO_DSP_BOOT_DIAGNOSTIC", 1)[1].split("#endif", 1)[0]
assert "g_SafeBootPolicy == SOF_SAFE_BOOT_FORCED_SAFE && !g_CanaryArmed" in stub
assert "WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device)" in stub
assert "SofStartFirmwareBoot" not in stub
assert add.index("WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device)") < (
    add.index("WdfDeviceInitSetPnpPowerEventCallbacks"))
print("PASS: halted ROM aborts before DMA start or FW_ENTERED acceptance; safe stub unchanged")
