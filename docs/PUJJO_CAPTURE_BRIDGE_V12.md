# Pujjo v0.0.12.0 — real capture bridge and ROM readiness evidence

## Target: a real Windows microphone, not another metadata endpoint

This checkpoint implements source changes for the actual WaveRT microphone
path, independently of the still-locked ROM candidate. The physical v8.1
endpoint was detection-only. v9.2 passed unpublished allocations and teardown;
v10/v11 integrated ownership and one-shot journaling. GitHub run 45976849887
passed the v11 WDK and host jobs. No firmware or capture DMA has executed on
the target board under this driver.

### PortCls CPU-page lease (interface v5)

The genuine DMIC route now requires interface version 5. Its PlanVoice and
AttachVoice operations separately negotiate exact ring size and borrow the
PortCls mapping. The child uses the stream-specific IPortWaveRTStream to
allocate whole dedicated pages and map them as cached CPU memory. It rejects
partial, chained or offset MDLs, zeros all accessible pages, and rolls back
allocation/map/attach failures. On free, the child stops/drains the parent's
CPU lease before unmapping and releasing pages through the same port interface.
It does not expose any HAL logical DMA address to user mode. The parent keeps
the four-slot hardware DMA ring, BDL and a separate short conversion scratch
buffer, converts only completed hardware periods into the CPU-only 48 kHz
mono signed 16-bit ring, and validates the sampled DMA position/timing before
publishing a packet. The parent never frees PortCls CPU pages.

The two-period conversion checks ring geometry and strictly increasing packet
timestamps, rejects overwritten/ambiguous periods, signals a reader on a
latched stream failure without publishing invented packets, and clears client
samples/counts on STOP/ACQUIRE. A lack of completions for two periods is
reported as a stream fault. GetReadPacket returns an I/O error for a faulted
stream rather than presenting silent data as a successful microphone.
The v3 detection interface and separate detection GUID are unchanged; a real
DMIC child refuses old interface versions. No analog route was opened.

The production methods are exercised using host page-allocation and parent
lease mocks, covering nine injected failures, exact size, duplicate requests,
wrong MDL, repeated free and reallocation. WaveRT conversion and parent route
tests cover data conversion, bounds, empty/overrun/faulted packet handling and
lifecycle. These are not WDK runtime checks or physical sample observations.

### Separate v12 diagnostic: no recording

A distinct guarded, demand-start INF and CI Release artifact
`Pujjo-v0.0.12.0-ROM-READINESS-UNSIGNED` are intended to collect the *next*
physical facts. The one-shot read-only diagnostic reuses the established
v6/v8/v9 diagnostic admission. It does not create a DMA enabler or IRQ, run
D0/firmware, invoke reset, change PCI configuration, attach an IOMMU domain,
publish a PortCls child, or configure any analog hardware.

It reports:
- PCI standard capability list validity, PCIe presence, advertised FLR,
  Transactions Pending, and PCI PM state (MAXULONG means unavailable).
  These are snapshots only. No PCIe capability is not proof of no reset
  support; the OS bus reset-interface type mask is the independent observation.
- The same cold-controller audit used by the compiled ROM loader, with
  PPCTL/foreign decoupled-stream mask and ADSPCS snapshot. The idle-audit
  status alone does not cover PPCTL, which is logged separately.
- Existing board identity, resource, NHLT/embedded route and bus reset
  interface evidence.

A capability bit or idle snapshot is not proof of a DMA drain. The v11
platform qualification hard gate remains false; no actual boot package is
authorized. Do not modify DMA remapping, DmaSessionDirty, PCI BME, or other
safety settings to bypass it. The submission is unsigned, and exact artifact
provenance, hardware-scope review and local signature/membership verification
are needed before any proposed read-only run. On the Chromebook, keep the
existing v9.2 disabled/disarmed until that review. Preserve WinRE and driver
recovery packages.

### Planned v13–14 acceptance milestones

- v13: use v12 physical evidence to resolve/review the ADL-N reset, DMA
  ownership and in-flight drain contract; only then design and execute a
  bounded one-shot pinned-ROM FW_READY/version and shutdown test. Repeated
  D0/D3 entry and surprise PnP behavior are not qualified.
- v14 target: after an actual successful ROM test, qualify DMIC IPC4 graph,
  actual DMA capture and stream start/stop/pause/resume, verify real
  non-synthetic samples through WASAPI or Sound Recorder, then Discord.
  No success is guaranteed by these version numbers. The hardware/mapping
  contract and actual FW_READY remain necessary.

References:
- https://learn.microsoft.com/windows-hardware/drivers/ddi/portcls/nn-portcls-iportwavertstream
- https://learn.microsoft.com/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertstream-allocatepagesformdl
- https://learn.microsoft.com/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavertinputstream-getreadpacket
- https://learn.microsoft.com/windows-hardware/drivers/audio/managing-memory-buffers-rebalance
- https://learn.microsoft.com/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard
