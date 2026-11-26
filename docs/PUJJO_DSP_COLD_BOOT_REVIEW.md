# Pujjo rev3 DSP cold-boot review

The signed v0.0.2.0 KMDF-only package is the recovery driver. The recorded device was healthy with Problem Code 0 and published INF `oem80.inf`. The previous boot-only artifact used INF DriverVer 0.0.3.0 and started in forced-safe mode as `oem81.inf` on the physical board. The physical v0.0.5.0 installation and failed one-shot authorization are recorded in [the local signing review](PUJJO_LOCAL_TEST_SIGNING.md). The replacement diagnostic uses 0.0.6.0. On the physical board, staging a previous candidate with the identical v0.0.2.0 INF returned the existing `oem80.inf` even though its SYS hash differed. The installed SYS remained the recovery baseline. A separate versioned INF and regenerated catalog address that identity collision; separate Driver Store publication and ranking must still be verified on Windows. Keep the service name and forced-safe defaults unchanged. Preserve the old package off-device and identify each package by its catalog, hashes and actual published name. Never remove the fallback to force staging; a newer equal-ranked package can be selected on a later enumeration. This version bump does not authorize installation or hardware boot.

## DMA lifetime hold (v0.0.6.0)

A one-shot request now clears the persisted token AND policy before authorization, and missing/consumed authorization selects the KMDF-only stub. The only authorized diagnostic callback path correlates PCI BAR/IRQ descriptors, maps and reads bounded capability registers, logs the observed PCI command, GCTL, PP/SPIB and stream counts, and unmaps. It never creates a DMA enabler or IRQ, registers a D0 callback, loads firmware, or creates an endpoint. The loader additionally rejects the diagnostic build before hardware access.

The DMA lifetime issue is not solved merely by keeping buffer handles or returning failure from ReleaseHardware: WDF common buffers are children of the DMA enabler and framework device teardown can retire them. The boot-capable implementation remains a development reference, NOT authorized for use. Before firmware execution, establish a supported stop/reset or independent DMA ownership strategy covering failed start, surprise removal and driver unload, and fault-inject failures through teardown. Do not remove the hold or re-arm older diagnostics. See [WdfCommonBufferCreate](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfcommonbuffer/nf-wdfcommonbuffer-wdfcommonbuffercreate) and [framework object lifetime](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/framework-object-life-cycle).

## Read-only preflight

1. Use the Release artifact named `Pujjo-DSP-boot-only-UNSIGNED-SUBMISSION` and the source checkout from the same Actions run. Require package version 0.0.6.0. Record the 40-character **build revision** from the run and the revision in the package notice. A PR build can use GitHub's test merge commit.
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

The post-DMA check now re-reads the cAVS Firmware Status Register after
both playback code-loader DMA and ICCMAX stop, rather than reusing the
earlier FW_ENTERED sample. If BAR4 disappears, ROM reports HALTED, the
state no longer equals FW_ENTERED, or the IPC boot session is faulted,
the loader rejects the boot before waiting for FW_READY and records
the before/after FSR samples plus ROM_ERR. This path remains behind the
existing experimental boot flags and one-shot authorization. Host-side
tests exercise the admission helper against synthetic register samples,
not the physical DSP.

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
