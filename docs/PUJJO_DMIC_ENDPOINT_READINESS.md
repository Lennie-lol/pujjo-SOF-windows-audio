# Pujjo internal microphone: Windows Sound endpoint milestone

**Goal:** the genuine Pujjo internal digital microphone enumerates under
Windows 11 **Settings → System → Sound → Input**. Enumeration is not
evidence that sample capture works; audio data must be separately verified.

## Working status

- The signed v0.0.2.0 **forced-safe DSP KMDF stub** starts with Code 0 on
  Google Pujjo rev3. It deliberately does **not** boot firmware or publish
  a microphone PDO.
- The separate gated DSP code contains DMA, ROM boot, IPC4 FW_READY,
  FW_CONFIG admission and DMIC child-publication logic, all physically unverified.
- The `pujjo_audio` PortCls WaveRT adapter creates an audio KS filter and
  physical capture connection for a **genuine** `PUJJO\AudioDmic` child
  after successful DSP initialization.
- `packaging/review/pujjo_dmic_only.inx` includes only the DMIC child
  (not SSP0 headset, SSP1 speakers, codec or amplifier hardware).
  It includes `ks.inf` and `wdmaudio.inf` registration and registers
  KS audio/capture/realtime and audio/topology interfaces.
- The CI review artifact is **unsigned** and NOT authorized for installation.

## Exact dependency chain

1. Verify physical Pujjo rev3 hardware, BAR0/BAR4 and IRQ resources without
   writing hardware; preserve the signed v0.0.2.0 fallback, hashes,
   offline recovery media and drive unlock key if applicable.
2. Qualify the pinned Intel-signed ADL-N SOF IPC4 firmware and topology on the
   physical board. Offline digest checks do **not** prove ROM acceptance.
3. Conduct an explicitly authorized, single guarded DSP boot attempt with
   proven rollback. Observe ROM FW_ENTERED, authentic IPC4 FW_READY and
   matching FW_CONFIG capabilities in logs for **that** attempt.
4. Verify a board-specific DMIC NHLT configuration/geometry and a working
   DMA/IPC4 capture pipeline. The software must not publish a DMIC child if
   firmware admission or format validation fails.
5. Sign and verify a reviewed *separate* `pujjo_audio` INF/SYS/CAT package
   under the target Windows kernel policy. Verify its sole hardware ID is
   `PUJJO\AudioDmic` and that it has no PCI/ACPI matches.
6. After controlled installation and parent publication, check Device Manager
   for the DMIC child, the Kernel Streaming interfaces, Windows AudioEndpointBuilder,
   and Settings → Sound → Input. Inspect SetupAPI, Code Integrity and PnP
   logs on failure. Do not force fake endpoint registration.
7. Record a real microphone signal and verify consistent capture positions,
   sample clock, notifications and D0/D3/surprise-removal recovery.

## Developer review package

After a successful WDK Release build on a **disposable CI runner**:

```powershell
$revision = (git rev-parse HEAD).Trim()
./scripts/Stage-PujjoDmicReview.ps1 -BinaryDirectory bin/x64/Release -OutputDirectory "$env:TEMP/pujjo-dmic-review" -SourceRevision $revision
./scripts/Verify-PujjoDmicReview.ps1 -SubmissionDirectory "$env:TEMP/pujjo-dmic-review" -ExpectedRevision $revision
```

It validates an **unsigned catalog for future signing**, not Windows kernel
loadability. No installer, firmware, activation flag, or unverified analog
endpoint is included. Do not stage this as a replacement for `oem80.inf`.

Reference: Microsoft's [Installing a Port Class Audio Adapter](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/installing-a-port-class-audio-adapter),
[Installing Device Interfaces for an Audio Adapter](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/installing-device-interfaces-for-an-audio-adapter)
and [Audio Endpoint Builder Algorithm](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-endpoint-builder-algorithm).
