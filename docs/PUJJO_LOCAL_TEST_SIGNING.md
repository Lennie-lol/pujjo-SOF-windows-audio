# Local catalog verification for the resource diagnostic

Current v0.0.7.0 scope: [native DMIC profile inspection](PUJJO_DMIC_PROFILE_V7.md).
Sign and verify only the exact CI submission from the corresponding revision. The
physical v0.0.6.0 diagnostic remains the known-good fallback.

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

The diagnostic DeviceAdd accepts the exact hardware ID from the INF. The preprocessor excludes the SDCA classifier and filter attach call. The HDA child, SoundWire child and filter creation routines no longer pin WDM objects or overwrite their reference counts.

DriverEntry publishes one driver-wide LONG permit only after flushing the persistent disarm. PrepareHardware claims it with InterlockedCompareExchange before board inspection, bus queries or mapping. Discovery failure consumes the permit. Release, rebalance and new device objects cannot renew it. Disarmed loads use the callback-free stub. Authorized devices register only prepare and release. Diagnostic D0 entry independently returns before any controller access. Discovery checks board and PCI identity, correlates resources, bounds capability reads and unmaps on success and rollback. The DMA, IRQ, D0, firmware, codec and amplifier paths remain closed.

Keep the hardware hold until both Windows configurations pass and the original Release artifact, exact checkout revision and member hashes are independently verified. A locally signed catalog also requires the separate membership and trust checks above.


## Physical v0.0.5.0 result and v0.0.6.0 replacement

On 2026-11-26 the physical Google Pujjo rev3 (PCI 8086:54c8, subsystem zero)
successfully installed the locally signed v0.0.5.0 CI binary (build checkout
`528e79b2570c3d1be9438c379001ab34735ebace`, SYS SHA-256
`85d91d5481189c542862e4cf26d8116b2829b2bac648cd32a90f3b41817c87aa`)
as `oem79.inf`. The disarmed device started; both signed fallbacks
`oem81.inf` (0.0.3.0) and `oem80.inf` (0.0.2.0) remained available.
A single authorized cold-start attempt emitted:

```text
[3] KMDF DriverEntry entered from FxDriverEntry!
[Safety] Canary not armed (Token=1, File=1)! Safe Mode active (zero BSOD).
[4] WdfDriverCreate returned 0x00000000
[5] SofEvtDeviceAdd entered!
[5-SafeStub] Forced-safe KMDF-only device creation; no DSP callbacks.
[5-SafeStub] KMDF device created without DMA, IRQ or firmware boot.
```

After boot the canary was absent and both authorization DWORDs were zero.
The physical probe did **not** execute. This is consistent with the two
registry writes succeeding but the subsequent flush failing; the exact
NTSTATUS was not captured, and the observations alone do not prove which
operation failed.

v0.0.6.0 separates the diagnostic authorization engine from the legacy
watchdog entirely. It attempts both resets independently for incomplete
or invalid requests; a reset failure cannot suppress the other reset.
It logs each actual write and flush NTSTATUS, verifies both readbacks,
requires exact STATUS_SUCCESS for the persistent operations, and grants
no permit on failure. It also checks the DWORD type/size and query bounds,
opens the diagnostic canary exclusively, logs open/delete/close statuses,
and identifies this revision in fresh breadcrumbs. Ordinary disarmed
starts do not flush the SYSTEM hive. The existing atomic prepare claim,
PCI identity, and hardware locks are unchanged. Host fault injection
executes the production disarm helper and exercises all status/readback
combinations, including the reported failure pattern (both writes
successful, flush failing).

**Do not interpret a successful host/CI run as a physical fix of the
unknown flush error.** Do not re-arm v0.0.5.0 or allow access without the
successful persistent flush. If the verified replacement still rejects
authorization, preserve the precise NTSTATUS and investigate; no
automatic retry, bypass, or relaxation is permitted.

A later, separately approved diagnostic may be started after Windows is
fully booted rather than during startup, to isolate an early-boot flush
problem. This is only a possible alternative, not an established fix:
first install the exact verified replacement disarmed, confirm Problem
Code 0, save the current log and signed fallbacks off-device, disable only
the exact PCI DSP, and confirm both the device is disabled and the
`sof_dsp` service is stopped/unloaded before creating a fresh one-shot
request. If the service remains resident, abort and re-enable disarmed;
do not force an unload or arm a resident driver. The next enable must
run a fresh DriverEntry, which still requires both resets, the successful
flush and zero readbacks before permission. Do not reboot with a leftover
request. A failure consumes the attempt: clear any remaining authorization
and preserve its logs before any further decision. Any subsequent reboot
must be disarmed.
