# Pujjo v0.0.9.2 — microphone hardware admission preflight

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
- Queries the Windows IOMMU v2 interface for the DSP PDO's available domain
  types. The diagnostic INF opts this device into Windows 11 24H2 remapping.
  The query logs NTSTATUS and the available domain mask, releases its token,
  and leaves domain attachment to Windows. Mask bit 0 indicates that a
  translation domain is available. Availability alone cannot establish that
  removing this driver's WDF buffers will revoke in-flight device writes.
- Creates a KMDF DMA enabler and concurrently allocates firmware payload and
  BDL, ICCMAX data and BDL, and 48 kHz four-slot 32-bit capture ring and BDL.
  The production common-buffer allocator requires the device-visible logical
  address to be aligned and within the reported HDA DMA width.
- Validates two-segment firmware and ICCMAX geometry and a two-period
  30720-byte capture ring (20 ms each), copies the payload using the CPU and
  checks it. No descriptor address is written to hardware. Releases all
  allocated buffers in reverse order and requests deletion of the enabler.
  The allocator counts successful WDF creates and actual destroy callbacks.
  The log records both counts after preflight and again in ReleaseHardware.
  Destruction may be deferred. Six creates and six destroys demonstrate
  framework cleanup of these unpublished buffers only.
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

## Experimental recording source

The firmware route uses the four-slot DMIC configuration above. The WaveRT
path averages the four signed 32-bit slots into mono 16-bit PCM at 48 kHz.
It copies a completed DMA period into private staging, checks the hardware
cursor and elapsed time, then publishes the period to a CPU-owned PortCls
ring. The packet interface reports the sequence and a cursor-derived QPC
timestamp. Debug and Release compilation and host-injected failures exercise
this code. Timing, slot polarity, amplitude and app behavior require hardware
evidence. This diagnostic package contains no recording endpoint.

## What remains before Discord

Safe DMA ownership and stop/abort/reset behavior through failed start,
PnP removal, rebalance and resume are still unqualified. Successful host
allocation does not prove hardware DMA quiescence. Firmware ROM acceptance,
FW_READY, mic-only IPC4 topology creation, capture position/notifications,
actual non-silent samples and app-compatible PCM formats all need physical
validation. Do not remove detection-only restrictions to make a fake
recording endpoint selectable.

## v0.0.9.2: independent backing-page experiment

A WDF common buffer's deletion is tied to the DMA enabler. Merely retaining
its handle in a per-device quarantine record cannot prove DMA safety on
surprise removal. Microsoft documents a different backend,
DMA_OPERATIONS.CreateCommonBufferFromMdl, in which the driver supplies
resident, system-mapped pages and owns freeing those pages independently
of the HAL common-buffer mapping.

The isolated preflight now requests DMA interface version 3 and uses the
borrowed WDM DMA adapter while its WDF enabler is still alive. After the
original WDF probe it tries six simultaneous independently backed buffers
with exactly the same firmware, ICCMAX and capture geometry. The MDLs use
fully allocated, physically contiguous, page-aligned cached memory so
systems without remapping can also attempt the test. The HAL supplies the
logical address; the code checks the entire rounded span including its
last byte, alignment, nonzero address and controller width. It never
substitutes a PFN or CPU address. Neither geometry validation nor copying
the pinned payload writes to the DSP.

Each successful map is paired with FreeCommonBuffer using the actual
round-up length, logical address and system VA before freeing MDL pages
and the MDL itself. All six are released in reverse order before requesting
enabler deletion. Missing versioned callbacks, partial allocation, failed
CPU mapping, failed HAL mapping, invalid logical span, or failed payload
comparison are reported, with cleanup of every acquired resource.

Host regression exercises the production allocator and stage for both
address widths, at each of six allocation positions and ten injected
failure types. It checks exact unmap arguments, reverse-order cleanup,
absence of leaks and unmap-before-page-free. These are simulated Windows
calls only; the WDK build and physical allocation result are separate
gates.

This is not yet a replacement for the active DMA backend: retaining the
pages independently does not prove an IOVA remains reserved, that the HAL
adapter remains usable after device removal, or that outstanding bus writes
have completed. No page or mapping is published to a descriptor. Active
DMA, firmware, child publication and recording stay compile-time locked.
A later active implementation must establish safe invalidation or
quiescence before freeing published pages or permitting rebind.

References:
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdlex
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-pfree_common_buffer
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfdmaenabler/nf-wdfdmaenabler-wdfdmaenablerwdmgetdmaadapter
