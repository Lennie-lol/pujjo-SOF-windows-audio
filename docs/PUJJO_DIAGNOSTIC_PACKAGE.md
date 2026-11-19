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

Before binding a signed diagnostic, export the Windows firmware NHLT from an elevated PowerShell session. Preserve the file and reported SHA-256 alongside the build revision, board revision, and exact instance IDs. The export returns an explicit unavailable result if Windows does not expose the table.

```powershell
.\scripts\Export-PujjoNhlt.ps1 -Path .\pujjo-windows-nhlt.bin
```

Use the resource diagnostic first and record its translated I2C, GPIO, and interrupt descriptors through kernel debug capture. Use the separately signed codec identity variant only after reviewing the resource capture and rehearsing rollback. It reads the codec ID in D0 and reports the value and status through kernel debug output. Opaque GPIO connection IDs identify resources but supply no SDB polarity evidence. Neither package contains a DSP, an audio endpoint, or runtime codec or amplifier control.
