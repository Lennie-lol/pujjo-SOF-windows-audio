#!/usr/bin/env python3
"""Regression contracts for existing WDK sender/DPC; not live DSP validation."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ipc = (root / "src/driver/Ipc.c").read_text()
send = ipc.split("NTSTATUS\nSofIpcSendMessage(", 1)[1].split("\n/**", 1)[0]
dpc = ipc.split("VOID\nSofIpcDpcHandler(", 1)[1].split("\n/**", 1)[0]
assert "Reply && (Reply->Payload || Reply->PayloadSize != 0)" in send
assert send.index("Reply &&") < send.index("WRITE_REGISTER_ULONG")
assert "Reply->PayloadSize = 0;" in send
assert "RtlCopyMemory(Reply->Payload" not in send
assert "pj_ipc4_mmio_span_valid" in send and "pj_ipc4_mmio_span_valid" in dpc
assert "Ctx->IpcTxBusy && !Ctx->IpcReplyReady && !Ctx->IpcFaulted" in dpc
assert "pj_ipc4_reply_matches(Ctx->IpcRequestPrimary, primary)" in dpc
assert "Ctx->IpcTxBusy = FALSE" not in dpc
assert dpc.index("SofDspWrite32(Ctx->Bar4VirtualAddress, Ctx->HwDesc->HipcTda") < dpc.index("KeSetEvent(&Ctx->IpcReplyEvent")
assert "Ctx->IpcFaulted = TRUE;" in send
assert "SofIpcDpcHandler(Ctx)" not in send
assert "SofDspDumpCore(" not in dpc and "SofIpc4DrainFwLogs(" not in dpc
assert "KeGetCurrentIrql() != PASSIVE_LEVEL" in send
logger = (root / "src/driver/Driver.c").read_text().split("void SofLogBreadcrumb(", 1)[1]
assert logger.index("KeAreAllApcsDisabled()") < logger.index("ZwCreateFile(")
print("PASS: header-only response contract, mapped range checks, reply ownership and timeout fault latch")

# Hardware busy must be checked while holding the software transmit lock and
# before touching either the mailbox or extension/primary registers.
assert send.index("KeAcquireSpinLock(&Ctx->IpcLock") < send.index("hostDoorbell = SofDspRead32") < send.index("Ctx->IpcTxBusy = TRUE;") < send.index("WRITE_REGISTER_ULONG")
assert send.index("hostDoorbell == 0xFFFFFFFFu") < send.index("hostDoorbell & Ctx->HwDesc->HipcIdrBusy")
assert "Ctx->IpcFaulted = TRUE;" in send.split("if (hostDoorbell == 0xFFFFFFFFu)",1)[1].split("}",1)[0]
for name,register in (("SofReadFwStatus","FwStatusReg"),("SofReadRomStatus","RomStatusReg")):
    body=ipc.split("ULONG\n"+name+"(",1)[1].split("\n}",1)[0]
    assert "!Ctx ||" in body
    assert body.index("pj_ipc4_mmio_span_valid") < body.index("SofDspRead32")
    assert "Ctx->HwDesc->"+register+", 4u" in body
print("PASS: hardware doorbell ownership and DSP status register bounds")

# Crash collection must not perform file I/O at elevated IRQL or read an
# unverified BAR range. Information omitted from a dump must not be fabricated.
dump=ipc.split("VOID\nSofDspDumpCore(",1)[1].split("\n/**",1)[0]
assert dump.index("KeAreAllApcsDisabled()") < dump.index("SofDspRead32")
assert dump.index("pj_ipc4_mmio_span_valid") < dump.index("SofDspRead32")
assert "Ctx->HwDesc->RomStatusReg, 32u" in dump
assert "Ctx->HwDesc->MailboxUplinkOffset, 4096u" in dump
assert "Ctx->HwDesc->MailboxDownlinkOffset, 4096u" in dump
assert "hdr->DeviceId = Ctx->DeviceId;" in dump
assert "hdr->VendorId = Ctx->VendorId;" in dump
assert "debugWinOffset + debugWinSize" not in dump
assert "ULONG debugWinSize = 0;" in dump
drain=ipc.split("VOID\nSofIpc4DrainFwLogs(",1)[1].split("\n/**",1)[0]
assert drain.index("pj_ipc4_mmio_span_valid(Ctx->Bar4Length, debugWinOffset, 65536u)") < drain.index("SofDspRead32")
assert "if (slotOffset != 0u) return;" in drain
assert "if (!slotOffset) return;" in drain
assert "if (dspWritePtr >= logBufSize || hostReadPtr >= logBufSize) return;" in drain
assert "consumed = bytesToRead;" in drain
assert "hostReadPtr = (hostReadPtr + consumed) % logBufSize;" in drain
assert "SofDspWrite32(sramBase, slotOffset, hostReadPtr);" in drain
assert "SofDspWrite32(sramBase, slotOffset, dspWritePtr);" not in drain
print("PASS: bounded diagnostic snapshots and trace cursor integrity")
