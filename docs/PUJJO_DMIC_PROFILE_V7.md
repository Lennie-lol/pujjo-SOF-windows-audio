# v0.0.7.0 — native microphone route qualification (not activation)

## Physical milestone already achieved

Lenn's physical Google Pujjo rev3, Windows 11, reported these results on
v0.0.6.0 (source revision 893fce3dc8f6401ec0ba4e887951d87c866baf1e):
the signed oem82.inf started disarmed, the driver unloaded when disabled,
then a single late-start canary successfully authorized a bounded resource
probe. The post-probe policy/token were both zero and the canary absent.

```
[8] Hardware identified: VID=0x8086 DID=0x54c8 -> Intel cAVS 2.5 (TGL/ADL/RPL)
[Probe] BAR0=0x4000 BAR4=0x100000 MSI=0 PCI_CMD=0x0006 GCTL=0x00000001 PP=0x800 SPIB=0x700 C7/P9
[Probe] Read-only resource discovery complete; DMA and firmware boot locked.
```

PnP reported IRQ 23 and BAR ranges DEEAC000–DEEAFFFF, DEF00000–DEFFFFFF.
This establishes resource discovery and one-shot disarm, not interrupt
delivery, firmware execution, DMA lifetime, or a working microphone.

## Changes

- A bounded, allocation-free production validator decodes the actual
  serialized DMIC0 gateway payload, rather than treating it as opaque
  nonempty aligned bytes. It verifies two PDM controllers, native
  four-slot PCM99 capture (48 kHz, 32/32 bits), the reviewed routing
  and control words, matching PDM clock dividers, clock bounds/duty
  cycle, CIC/FIR decimation, exact 48 kHz arithmetic, FIR length,
  coefficient width, nonzero filters and exact end of payload.
- Native NHLT admission performs a full checksum/bounds walk and
  exact-format match, rejects conflicting records and validates the
  matched payload without copying. The same production validator checks
  the payload again before creating the DMIC IPC4 graph. No arbitrary
  stereo downgrade is admitted, since the recovered PCM99 evidence is
  four slots; Windows exposes slots without inferring physical mic count.
- The parent now verifies the 768 bytes/ms module budgets at publication,
  with overflow-safe pipeline page bounds, and WaveRT refuses the wrong
  native format before allocation. No fake or analog endpoint is added.
- The v0.0.7.0 diagnostic uses the same native table inspection after the
  physical read-only PCI probe and BAR unmap. It logs NHLT availability,
  structure result, clock/decimation and mailbox fit. It never consults
  a replacement table, submits IPC, allocates DMA, registers an IRQ or
  publishes an audio child. Missing NHLT does not invalidate the completed
  PCI resource probe. A structurally valid profile does not authorize it.

## Independent evidence and scope

The exact serialized fields are based on the existing pinned ALSA v1.2.13
encoder comparison and SOF v2.12 TGL/ADL 38.4 MHz IO clock. CI generates
the reference twice, independently audits the payload, then runs the new
production C implementation against that real reference and the complete
candidate NHLT. CI also emits a separate DMIC-only candidate with one
endpoint and no SSP/SoundWire records and verifies it using the production
parser. It is review material only, not a fallback header. Synthetic tests exercise truncated, unaligned, modified,
and oversized input, including rejection of reserved fields, incompatible
clocks, malformed FIR data, conflicting formats and module-budget overflow.
These are offline checks, not measured clocks or firmware acceptance.
See [recovery evidence](../topology/pujjo/DMIC_RECOVERY_EVIDENCE.md).
Microsoft's [AuxKlib firmware-table API](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/aux_klib/nf-aux_klib-auxklibgetsystemfirmwaretable)
is used for the native table query.

## Remaining release gates

The signed v0.0.6.0 resource-only driver is the known-good physical
diagnostic. The v0.0.7.0 CI submission is unsigned. Preserve v0.0.2.0 and
v0.0.6.0, WinRE, and the local signing verification procedure. The DMA
lifetime hold remains: KMDF child common buffers can be retired with
their parent device and no supported removal-safe DMA ownership or
quiescence proof has yet been established. No boot attempt or input
endpoint install is authorized by this version. Firmware ROM authentication,
FW_READY/FW_CONFIG, native or separately qualified DMIC-only fallback
geometry, physical sample capture and power/remove lifecycle are separate
milestones. Headphone and speaker routes remain independently locked.
