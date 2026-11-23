# Pujjo resource diagnostic package

The Release workflow stages two independent x64 Windows 11 24H2 diagnostic packages for the RT5682S and RT1019P ACPI nodes. Each package contains its probe binary, INF, and unsigned catalog. The workflow runs WDK InfVerif and Inf2Cat and records the build revision and file hashes. The diagnostic builds report the translated assignments through kernel debug output. The same workflow also stages a separate optional RT5682S identity-only submission with the guarded SPB read enabled. The two codec variants use the same ACPI ID and must be staged one at a time. The packages leave the Intel DSP and Serial IO drivers alone.

## Signing and verification

Use the authorized Microsoft Hardware Dev Center workflow applicable to the intended Windows edition and policy. Submit the exact generated driver package as required by Partner Center and use the returned package. Validate the final INF and verify both the catalog and its membership against the Windows kernel policy with the WDK SignTool. Record the final revision and hashes separately from the unsigned submission.

```powershell
signtool verify /kp /v .\pujjo_rt5682_probe.cat
signtool verify /kp /v /c .\pujjo_rt5682_probe.cat .\pujjo_rt5682_probe.inf
signtool verify /kp /v /c .\pujjo_rt5682_probe.cat .\pujjo_rt5682_probe.sys
```

Repeat for the amplifier package. Keep Secure Boot and driver signature enforcement enabled. The GitHub artifact is submission input. A package returned by the authorized signing workflow is required for installation.

## Recovery and rollback

Before installing a signed diagnostic, record the exact codec and amp PnP instance IDs, current driver associations, and current published driver-store names. Save recovery access and export any driver packages for those instances beforehand.

```powershell
pnputil /enum-devices /instanceid "<exact ACPI instance ID>" /drivers
pnputil /export-driver <existing-oemNN.inf> C:\PujjoRecovery
```

On a recovery-ready system, authorize and stage one signed package at a time. Capture the newly assigned published name from PnPUtil and confirm the instance association. After the diagnostic, remove only that newly recorded probe package, rescan devices, and confirm the original association. If needed, reinstall the exported prior package.

```powershell
pnputil /delete-driver <new-probe-oemNN.inf> /uninstall
pnputil /scan-devices
pnputil /enum-devices /instanceid "<exact ACPI instance ID>" /drivers
```

If normal startup fails, use Windows Recovery Environment and identify the offline Windows volume before inspecting third-party drivers with `dism /Image:<WindowsVolume> /Get-Drivers`. Remove only the matching probe INF with `dism /Image:<WindowsVolume> /Remove-Driver /Driver:<new-probe-oemNN.inf>`. Rehearse recovery against a disposable Windows image before binding a signed package on the Chromebook.


## Read-only evidence collection

Before binding a signed diagnostic, export the Windows firmware NHLT from PowerShell. Preserve the file and reported SHA-256 alongside the build revision, board revision, and exact instance IDs. The export returns an explicit unavailable result if Windows does not expose the table.

```powershell
.\scripts\Export-PujjoNhlt.ps1 -Path .\pujjo-windows-nhlt.bin
```

Use the resource diagnostic first and record its translated I2C, GPIO, and interrupt descriptors through kernel debug capture. Use the separately signed codec identity variant only after reviewing the resource capture and rehearsing rollback. It reads the codec ID in D0 and reports the value and status through kernel debug output. Opaque GPIO connection IDs identify resources but supply no SDB polarity evidence. Neither package contains a DSP, an audio endpoint, or runtime codec or amplifier control.

## DSP boot-only submission

A separate guarded x64 build defines PUJJO_DSP_BOOT_DIAGNOSTIC alongside
the DSP boot and reviewed firmware compile flags, never the audio/analog
or replacement NHLT flags. Its INF uses forced-safe defaults. PrepareHardware
requires a successfully consumed one-shot persistent canary; D0Entry consumes
the in-memory authorization before invoking the loader. Failed canary file
disposition, failed token write or failed registry flush does not arm the
single-shot path. Existing hardware identity checks and loader digest gates
remain in place. No package changes hardware state during CI.

The Release artifact contains INF, SYS, unsigned catalog and hashes. It
does not contain a firmware binary, installation/arming script, or approved
replacement topology. The exact pinned ADL-N firmware is a separate
prerequisite. Microsoft signing, final catalog checks, recovery rehearsal,
physical hardware review and explicit authorization remain blockers to use.

The four-route 2453-byte NHLT candidate passed CI #119, including real
parser lookups, independent reference encoder and the 768-byte firmware
catalog budget. This is offline qualification, not confirmation of actual
clocks, physical microphone array, codec identity, GPIO polarity or SSP
timing. No evaluated hardware profile is promoted in this submission.

The boot-only diagnostic now has a dedicated exact SMBIOS guard for Google
Pujjo rev3 (not Pujjo1e or another board revision), requiring a single valid
Type 2 record and end-of-table marker. The PCI subsystem IDs must both be
zero, matching the recorded Windows inventory. This narrows the diagnostic
to the documented target; identity checks do not establish physical safety.
Existing family-level guards for other development variants are unchanged.
No signing, hardware activation or firmware boot is performed by CI.


## Read-only DSP boot-only artifact verification

The Release CI job now runs scripts/Verify-PujjoDspBootDiagnostic.ps1 after staging the unsigned package and before upload. It checks the three package members against sha256.json, the source revision, Pujjo PCI identity, and forced-safe INF values. It does not install, sign, arm, boot or touch hardware.

On Windows after extracting the unsigned GitHub Actions artifact, run:

    .\scripts\Verify-PujjoDspBootDiagnostic.ps1 -SubmissionDirectory .\Pujjo-DSP-boot-only-UNSIGNED-SUBMISSION

Optionally pass -FirmwarePath to verify the exact ADL-N IPC4 image, and -InspectLocalBoard on the physical Google Pujjo rev3 for read-only SMBIOS and PCI checks. After obtaining an authorized Microsoft-signed package, use -SignedPackageDirectory and -SignToolPath to verify that INF and SYS remain identical to CI and that the CAT has valid Windows kernel-policy signature and membership (SignTool /kp). The preflight result is not hardware qualification and must not be used to authorize installation or one-shot DSP boot. Signed package acquisition, reversible rollback rehearsal, and physical DMA/IRQ review remain separate requirements.
