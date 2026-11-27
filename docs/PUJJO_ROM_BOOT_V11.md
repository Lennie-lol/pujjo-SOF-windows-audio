# Pujjo v0.0.11.0 — guarded ROM-boot source checkpoint

**Source only. No new package is authorized for installation or hardware
activation.** The physical v9.2 result established unpublished allocation
and release, not execution or safe DMA teardown after device loss.
Keep oem85.inf disarmed/disabled; retain the v8.1 detection recovery exports.

## Implemented

- One driver-wide DMA admission attempt and a persistent dirty journal.
  In a future qualified candidate, admission requires an existing DWORD
  `DmaSessionDirty` equal to zero. Before publication, the driver writes
  one, flushes the key and reads back one. Any failure consumes the attempt.
  There is no INF default, watchdog, ordinary restart, or unload path that
  resets the journal. It is not an activation instruction; no journal
  initialization or recovery policy is authorized at this stage.
- A single atomic descriptor ledger combines admission, closure,
  invalidation and up to 30 descriptor ownership bits. Ownership is claimed
  before either half of a BDL address is written. Duplicate claims and
  claims after closure fail. Orderly stop permits an existing owner to
  retire a descriptor after checked detach; surprise loss invalidates
  retirement permission while preserving outstanding bits. Rebalance does
  not adopt a prior device generation's claims. This is a software
  ownership record, not a bus fence.
- Before changing global processing registers, the cold-boot path audits
  all advertised input/output/bidirectional descriptors, RUN/reset/BDL
  state, command and response DMA engines and base addresses, position
  buffer, flush state, powered DSP cores and foreign PP stream bits.
  Unexpected state fails closed. No assumption of exclusive ownership
  follows merely from loading the PCI driver.
- A new CI-only ROM candidate compiles the genuine firmware loading path
  without audio activation or analog flags. Its planned successful path
  ends after FW_READY/version IPC, closes publication, quiesces IPC, powers
  down the DSP and checks controller restoration and ownership. It does not
  publish the microphone/audio child or run the ongoing audio/dump/watchdog
  session. DriverEntry still refuses this compile-only variant.
- Independently, the platform DMA qualification check deliberately returns
  false before journal admission or active DMA setup. It cannot be bypassed
  with existing canary or registry safety flags.

## Verification scope

Host regression uses the production admission and descriptor-ownership
conditions, including simultaneous journal starts, duplicate descriptor
claims, competing closure and claim, surprise invalidation and retirement,
registry/readback/flush faults, cold-restart dirty state, foreign descriptor
addresses, bidirectional descriptors, occupied cores and foreign controller
DMA state. The CI configuration adds a distinct WDK ROM-only compilation
alongside the existing variants. Host tests do not exercise actual PCI,
Windows PnP, DMA arbitration, interrupt timing or ROM authentication.

## Remaining hardware admission blocker

The v9.2 transcript reports available IOMMU domain mask 0x3 but no attached
domain; the reset interface reports types 0x00000000 even though a handler
pointer was present. Neither establishes function reset support or a
platform isolation/drain contract. A software check immediately before an
MMIO access cannot guarantee the device remains present during the access.
A successful RUN-clear/reset on a responsive descriptor is not a
surprise-removal fence. Keeping a dirty registry value and retained pages
also cannot reserve an IOVA after adapter teardown, or prevent a different
driver or OS from binding. Intel HDA's FCTL flush procedure requires a
valid DMA position buffer and stopped streams; it is not a surprise-loss
remedy. Do not use global HDA reset, domain attachment/detachment, or DMA
publication as a workaround. Continue reviewing the device/platform DMA
ownership and drain contract before an active experiment.

Optional read-only observation with the current hardware configuration:

```powershell
$id = 'PCI\VEN_8086&DEV_54C8&SUBSYS_00000000&REV_00\3&11583659&0&FB'
Get-PnpDevice -InstanceId $id | Format-List Status,FriendlyName,InstanceId
Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_DmaRemappingPolicy' -ErrorAction Continue |
    Format-List KeyName,Type,Data
```

An absent/zero property does not establish enforced remapping; a value of
2 denotes driver support, not proof of an attached domain or drained
traffic. Record the result without changing policy or enabling the device.

The next acceptance milestone, **only after the remaining safety contract
is qualified**, is a bounded one-shot pinned SOF IPC4 ROM boot, FW_READY and
version query with demonstrated safe shutdown. Then independently qualify
the DMIC route, capture DMA lifecycle, and actual WaveRT/WASAPI recording.
No microphone samples, Discord compatibility or speaker/amp activation are
claimed.

References:
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_reset_interface_standard
- https://learn.microsoft.com/windows-hardware/drivers/pci/enabling-dma-remapping-for-device-drivers
- https://learn.microsoft.com/windows-hardware/drivers/display/iommu-dma-remapping
- https://learn.microsoft.com/windows-hardware/drivers/kernel/handling-an-irp-mn-surprise-removal-request
- Intel High Definition Audio Specification, section 3.3.7 (Flush Control).
