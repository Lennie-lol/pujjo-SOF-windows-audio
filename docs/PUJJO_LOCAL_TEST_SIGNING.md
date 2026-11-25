# Local catalog verification for the v0.0.5.0 resource diagnostic

Use Windows PowerShell on the controlled Chromebook. Obtain the original `Pujjo-DSP-boot-only-UNSIGNED-SUBMISSION` ZIP directly from the known successful PR #29 Release job. Extract it separately from the directory containing the locally signed CAT and the two unchanged CI members. Obtain the full checkout SHA from that build's checkout log (a PR merge SHA is possible). The manifest and notice in a user-supplied directory cannot authenticate their own origin.

From the source checkout, set the paths below to the actual local paths. Supply Microsoft's installed Windows SDK SignTool. The script only reads the package, board and firmware.

```powershell
$ci = Read-Host 'Extracted original Actions artifact directory containing sha256.json'
$signed = Read-Host 'Directory containing only the test-signed CAT, INF and SYS'
$revision = Read-Host 'Full checkout SHA from the original Release job'
$firmware = Read-Host 'Pinned ADL-N firmware file'
$tool = Read-Host 'Full path to the installed Windows SDK signtool.exe'
.\scripts\Verify-PujjoLocalDiagnostic.ps1 -SubmissionDirectory $ci -SignedPackageDirectory $signed -ExpectedRevision $revision -FirmwarePath $firmware -InspectLocalBoard -SignToolPath $tool
```

Require the final local PASS. The inherited preflight prints its unsigned-submission status before the local checks run. An exception at any stage ends verification. The local checks require the trusted CAT signer thumbprint `9B7476DECA7CA559F9B77CA9DEE19E38B9A9E9E5`, successful SignTool Authenticode verification of the CAT, explicit membership of each member in that CAT, and byte-identical INF/SYS. For public releases the existing signed-package verifier still requires SignTool kernel policy for the CAT and both members. A self-signed local PASS cannot satisfy that gate.

## Hardware review of PR #29 at 91e7ed7

**Hold physical installation and arming of this artifact.** Its diagnostic INF only matches the PCI DSP and registers no SDCA filter, but a prior device or class filter association with the same service could still deliver an SDCA DeviceAdd. The following findings apply to the compiled source even if a fresh board currently presents only one matching DSP.

- In `Driver.c:SofEvtDeviceAdd`, the armed SDCA branch precedes the diagnostic PCI guard. If invoked for that device it calls `SofSdcaFilterDeviceAdd`. That function in `SdwController.c` installs IOCTL handling and directly overwrites a framework device object's reference count. Safe filter teardown cannot be established. A future diagnostic build should exclude this route at compile time.
- DriverEntry consumes the persistent token and writes and flushes forced-safe policy before granting the in-memory lease. In `Device.c:SofEvtDevicePrepareHardware`, the process-wide flag uses a separate read and clear. Concurrent callbacks on distinct device objects could both pass the check. Use a single interlocked claim before any discovery and cover concurrent start, resource rebalance and failed-start/re-add in regression tests.
- On the intended PCI path, config is read through a referenced bus interface and that reference is released. Raw and translated descriptors are correlated before mapping. Capability traversal checks bounds, alignment and cycles. Success and partial-map or capability failures converge on an unmap helper that clears pointers and lengths. Diagnostic ReleaseHardware can call it again without touching IPC or stream state.
- The diagnostic PCI branch returns before creating IRQ or DMA objects. It registers no D0 callbacks, programs no streams, and reaches no codec, amplifier or firmware execution. The separate firmware loader guard stays in force. The full loader's DMA-buffer lifetime across failed start and removal remains unqualified.

Keep the working fallback bound. Existing board, recovery, backup and signing observations do not exercise either failing path. Installation, one-shot arming and physical rollback procedures require a reviewed replacement binary and a newly verified catalog. Do not apply the cold-boot evidence procedure to this artifact.

## Replacement source review for v0.0.5.0

The diagnostic DeviceAdd accepts the exact hardware ID from the INF. The preprocessor excludes the SDCA classifier and filter attach call. The SoundWire child and filter creation routines no longer pin WDM objects or overwrite their reference counts.

DriverEntry publishes one driver-wide LONG permit only after flushing the persistent disarm. PrepareHardware claims it with InterlockedCompareExchange before board inspection, bus queries or mapping. Discovery failure consumes the permit. Release, rebalance and new device objects cannot renew it. Disarmed loads use the callback-free stub. Authorized devices register only prepare and release. Discovery checks board and PCI identity, correlates resources, bounds capability reads and unmaps on success and rollback. The DMA, IRQ, D0, firmware, codec and amplifier paths remain closed.

Keep the hardware hold until both Windows configurations pass and the original Release artifact, exact checkout revision and member hashes are independently verified. A locally signed catalog also requires the separate membership and trust checks above.
