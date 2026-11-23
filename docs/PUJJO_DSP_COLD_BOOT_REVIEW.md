# Pujjo rev3 DSP cold-boot review

The signed v0.0.2.0 KMDF-only package is the recovery driver. The recorded device was healthy with Problem Code 0 and published INF `oem80.inf`. The boot-only review artifact uses the same version. Identify each package by its catalog, hashes and actual published name.

## Read-only preflight

1. Use the Release artifact named `Pujjo-DSP-boot-only-UNSIGNED-SUBMISSION` and the source checkout from the same Actions run. Record the 40-character **build revision** from the run and the revision in the package notice. A PR build can use GitHub's test merge commit.
2. On the board, capture these queries and the result of the repository verifier. Supply paths to the extracted artifact and the separately obtained firmware. The verifier checks the candidate's 707036-byte length and SHA-256.

   ```powershell
   $dsp = @(Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_8086&DEV_54C8&SUBSYS_00000000*')
   if ($dsp.Count -ne 1) { throw 'Expected one present Pujjo DSP' }
   $dsp | Format-List Status,InstanceId
   Get-PnpDeviceProperty -InstanceId $dsp[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode,DEVPKEY_Device_DriverInfPath,DEVPKEY_Device_DriverVersion
   pnputil /enum-devices /instanceid "$($dsp[0].InstanceId)" /resources
   .\scripts\Verify-PujjoDspBootDiagnostic.ps1 -SubmissionDirectory $artifact -ExpectedRevision $revision -FirmwarePath $firmware -InspectLocalBoard
   ```

3. Confirm Google / Pujjo / rev3, the recorded Windows and UEFI environment, preserved signed fallback and recovery access. Reconcile differences in INF, problem code, resource assignment or identity before proposing a test. The verifier can additionally check a separately signed catalog and its INF/SYS membership using `-SignedPackageDirectory` and `-SignToolPath`.
4. Review the assigned memory and interrupt resources and the platform DMA-remapping configuration. The guarded loader requires working BAR0/BAR4, PCI memory decode and bus mastering, a controller already out of reset, a free playback slot and capture slot, PP/SPIB, and a connected interrupt. Windows resource enumeration alone cannot verify register readback or interrupt delivery. Obtain explicit approval for a single physical attempt after signing and recovery rehearsal.

## Evidence from an authorized attempt

Record UTC start/end times, exact device instance, package hashes, firmware hash and Windows build. Preserve the pre-attempt length of `C:\sof_entry.txt` so older messages cannot satisfy this run. Collect its appended bytes, SetupAPI.dev.log and the Windows System / Kernel-PnP events for the same interval. If a kernel-debug capture is already available, record raw IPC notification and reply headers there. The file logger skips DPC-level calls.

| Stage | Evidence required |
| --- | --- |
| Image and resources | Identity, translated BAR lengths, IRQ type, stream counts, accepted image and payload length |
| Core and ROM | Core 0 reset, stall and power readbacks, PURGE acknowledgment, INIT_DONE before playback RUN |
| DMA | Stream number/tag and SPIB length, RUN acknowledgment, FSR state code 5 (FW_ENTERED), successful stop and BDL detachment |
| IPC4 | Cold notification primary `0x1b080000`, extension `0`; acknowledged notification; successful base-firmware FW_CONFIG GET with matching four-part version, both mailboxes at least 4096 bytes and at least two pipelines |
| Completion | `[12g]` boot completion and successful D0 entry from this attempt. Audio endpoints and playback are outside this milestone. |

Record the **first failing stage** and its NTSTATUS. Preserve raw FSR, ROM_ERR, LPIB and SD_STS when available. HALTED is FSR bit 31 on accessible MMIO. All-ones MMIO indicates an unavailable register. A timeout before PURGE acknowledgment can expose retained SRAM values. A lost IRQ, malformed reply, version/capability mismatch, or unconfirmed stop/restore ends the attempt. A digest match establishes host file identity. ROM acceptance and the running firmware response require board evidence.

## Recovery

Before testing, keep the signed fallback, its hashes, recovery media, any required volume unlock key, and the exact prior device association accessible off the test installation. Rehearse driver recovery on a disposable Windows image. Record the new experimental package's published INF separately when a future installation is authorized.

After a failed attempt, stop testing and preserve the logs. For an unconfirmed DMA stop, do not retry, rebind or reuse buffers in that running session. Use the rehearsed shutdown/cold recovery path. If Windows starts, restore the saved signed fallback to the recorded device and verify its catalog and binding. If Windows fails to start, boot WinRE, identify and unlock the actual Windows volume and list its third-party packages with `dism /Image:<offline-volume> /Get-Drivers`. Identify the experimental package by its recorded published name before using the rehearsed offline removal procedure. Preserve the recovery package throughout. After recovery verify the original signed bytes, driver association, forced-safe policy, Problem Code 0 and absence of new hardware-boot breadcrumbs.

## Source basis

Linux selects the TGL descriptor for ADL-N and uses a capture stream for ICCMAX during code loading. The reference loader polls ROM FW_ENTERED. SOF 2.12's cold boot producer sets the complete primary and extension words, and its base firmware reports FW_CONFIG as TLVs. The physical board must establish the starting controller state, DMA visibility with remapping, ROM acceptance, and IRQ timing. [Linux PCI selection](https://github.com/torvalds/linux/blob/master/sound/soc/sof/intel/pci-tgl.c), [pinned Linux loader](https://github.com/thesofproject/linux/blob/b4bc873561db55f8ed1bb4113797d768424f300d/sound/soc/sof/intel/hda-loader.c), [SOF IPC4 producer](https://github.com/thesofproject/sof/blob/v2.12/src/ipc/ipc4/handler.c), [SOF base firmware](https://github.com/thesofproject/sof/blob/v2.12/src/audio/base_fw.c).
