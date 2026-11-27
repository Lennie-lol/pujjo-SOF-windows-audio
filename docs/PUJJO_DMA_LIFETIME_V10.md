# Pujjo v0.0.10.0 — source-only DMA ownership integration

## Physical evidence received 2026-11-27

Google Pujjo rev3, Intel 8086:54c8; v0.0.9.2 test-signed parent INF
oem85.inf (CI revision 9368fecf305505548b5d88e37f58dccb4d73b16b).
The user completed the one-shot DMA admission probe and subsequently disabled
the PCI function. The supplied log reports:

- BAR0 0x4000, BAR4 0x100000; legacy IRQ, MSI=0.
- The pinned image parsed; 704512-byte payload, copier budget two pages and
  gain three pages; embedded 1848-byte 2.4 MHz PDM route validated but not
  activated. Native NHLT was absent (0xc0000225).
- IOMMU query success, domain availability mask 0x3; no domain attached.
  This is availability, not confirmation of an active translation domain.
- Six WDF buffers and six independently owned MDL-backed mappings allocated
  and checked without publishing any descriptor address. Capture geometry
  30720 bytes, 20 ms/period. Mapping deletion preceded backing page release.
- Admission status 0x00000000; ReleaseHardware WDF created=6 destroyed=6.
  Device disable succeeded and the service was STOPPED; the reported
  WIN32_EXIT_CODE was 31, whose cause was not established.
- The canary was consumed; persistent safety parameters returned to zero.
  Firmware execution, hardware DMA and recording were never started.

Keep the device disarmed. v0.0.8.1's separately exported detection packages
are a recovery/reference baseline, not working microphone drivers.

## v10 changes

The active source now shares the MDL-backed allocator exercised by v9.2.
Firmware payload/BDL, ICCMAX data/BDL and DMIC capture ring/BDL use
driver-owned MDLs, with explicit device DMA directions and HAL logical
addresses. The active enabler requests WDM DMA version 3. Analog buffers
retain their existing framework allocation and lease design.

Publication ownership is recorded before either BDL half can reach the
controller. A partial code-loader configuration now records the actual
descriptor slot and routes through checked teardown rather than freeing
potentially published pages. Boot, ICCMAX and capture stop/reset paths
reject inaccessible or foreign descriptor addresses. Boot and ICCMAX
require bounded RUN-clear, reset assertion/deassertion and detached BDL
readback before release. Capture requires the same ownership/readback
conditions. A free request on a published buffer faults the session and
refuses to unmap or release it. Retained slots cannot overwrite another
owner; a collision or out-of-range slot remains a fatal fault, not a
recoverable allocation failure. These paths are host-testable source,
not proven hardware stop guarantees.

The regression harness exercises the real allocator wrapper, published
free refusal and session fault latch; descriptor fault injection covers
foreign/partial address programming, missing stop/reset acknowledgement
and lost register access. The candidate boot gate stays closed.

## Remaining blocking safety issue

Driver-owned backing pages can outlive WDF object teardown, but the
borrowed adapter and its logical mappings cannot be assumed to remain
valid or reserved across PnP stop/removal. A leaked MDL is not proof that
an IOVA cannot be reused. A descriptor reset acknowledgement on a
responsive device is not a proof of bus-master quiescence on surprise loss.
The driver must obtain a defensible platform DMA isolation/drain contract
and prevent rebind/reuse before attempting an active ROM transfer.
Do not attach/detach IOMMU domains on a running device as a shortcut:
Microsoft's documented domain-switch operations are not atomic, require
silent hardware, and unmapping or flushing an IOMMU does not by itself
establish completion of all in-flight device writes.

v10 does not unlock firmware boot or add a recording endpoint. No
installation authorization, DMA execution, codec/amp activation, samples
or Discord compatibility is claimed. The v9.2 preflight, v8.1 detection
and other existing diagnostic package versions retain their scope.
The source-only candidate is not an installable v10 binary.

References:
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-pcreate-common-buffer-from-mdl
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-pput_dma_adapter
- https://learn.microsoft.com/windows-hardware/drivers/display/iommu-dma-remapping
