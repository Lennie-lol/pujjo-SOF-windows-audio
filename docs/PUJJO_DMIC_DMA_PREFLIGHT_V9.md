# Pujjo v0.0.9.0 — microphone hardware admission preflight

**Source candidate. Not a recording driver. No installation or activation authorization.**

## Physical evidence from v0.0.8.1 (2026-11-27)

On Google Pujjo rev3, the signed detection parent and child successfully
published the DMIC-only PDO and a Windows capture endpoint with DeviceState=1.
Windows All sound devices showed it enabled; the classic Recording page
listed it. The main Sound input selector excluded it, as intended by the
non-default detection-only endpoint policy. Its stream creation deliberately
returns DEVICE_NOT_READY. No samples were captured.

The one-shot resource probe consumed the canary and persistently disarmed.
Observed BAR0 0x4000, BAR4 0x100000, legacy IRQ, 7 capture and 9 playback
streams, PP 0x800, SPIB 0x700. Native ACPI NHLT absent (0xc0000225);
the embedded 1848-byte 2.4 MHz PDM-only profile passed validation.
The bus reset interface was present, with supported reset types zero.
This is not proof of a safe reset or of DMA ownership during device loss.

## Next narrow test

The new isolated parent build, PUJJO_DMIC_DMA_PREFLIGHT, still uses the
diagnostic compile-time firmware and D0 locks, exact board identity and
driver-wide one-shot authorization. It does not publish a microphone child.
After the resource probe unmaps both BARs, it:

- Reads and hashes the exact pinned 707036-byte ADL-N IPC4 firmware.
- Parses the signed-payload metadata (does not authenticate via DSP ROM).
- Checks a native DMIC profile or, only if native NHLT is absent, the
  embedded reference. Verifies copier/gain module budgets and mailbox fit.
- Creates a KMDF DMA enabler and concurrently allocates firmware payload and
  BDL, ICCMAX data and BDL, and 48 kHz four-slot 32-bit capture ring and BDL.
  The production common-buffer allocator requires the device-visible logical
  address to be aligned and within the reported HDA DMA width.
- Validates two-segment firmware and ICCMAX geometry and a two-period
  15360-byte capture ring (10 ms each), copies the payload using the CPU and
  checks it. No descriptor address is written to hardware. Releases all
  allocated buffers in reverse order, requests deletion of the enabler.
- Never invokes firmware boot, DMA transactions, IRQ creation, production
  PortCls publication, codec or amplifier control.

Host tests fault-inject each allocation, zero addresses, invalid geometry,
and payload comparison failure. They assert complete reverse-order cleanup.
Physical allocation and Windows framework behavior are not established by
these tests. Verify the Windows build and review the final binary before
planning a controlled test. Preserve the v8.1 endpoint evidence and v6
recovery package, and keep the Chromebook disarmed until then.

The unsigned package includes the existing release and local-signature
verifiers with the explicit -DmaPreflight switch. Local signing is performed
on the VM, with the pinned development certificate; the Chromebook does
not need SignTool. Microsoft release signing remains a different gate.

## What remains before Discord

Safe DMA ownership and stop/abort/reset behavior through failed start,
PnP removal, rebalance and resume are still unqualified. Successful host
allocation does not prove hardware DMA quiescence. Firmware ROM acceptance,
FW_READY, mic-only IPC4 topology creation, capture position/notifications,
actual non-silent samples and app-compatible PCM formats all need physical
validation. Do not remove detection-only restrictions to make a fake
recording endpoint selectable.
