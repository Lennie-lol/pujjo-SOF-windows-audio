# Pujjo SOF — Preinstallation readiness and evidence (development, NOT an installer)

## Current status

**Not installable. Not hardware verified. No working speaker or microphone audio.**

The Windows 11 VM independently passed Debug/Release and eight static tests
at commit `d990314c`, reported in user-provided `RESULTS.txt`. GitHub Actions
also passed all three jobs at `main` in [run #22](https://github.com/Lennie-lol/pujjo-SOF-windows-audio/actions/runs/44271833015)
(commit `3ad0653e`) and tested the later combined read-only discovery and
BAR rollback changes at [run #24](https://github.com/Lennie-lol/pujjo-SOF-windows-audio/actions/runs/44272395075)
(commit `0cb3d5e3`, **38/38 offline commands, Debug and Release all passed**).
Those combined changes were then merged to `main` at `58fad987`.
Check the newest `main` workflow result separately; a past successful run
does not establish a subsequent commit's CI status.

This evidence proves compilation, not functional device initialization, a
valid installable driver package, real SOF firmware boot, or audio playback.

The subsequent development revision `4831d721e1af3bbbcba6907474f4aed432c3704f` passed all three jobs in [run #69](https://github.com/Lennie-lol/pujjo-SOF-windows-audio/actions/runs/44368075960), including WDK Debug/Release and portable regressions. The consolidation retains that source tree (with documentation updates); the consolidation commit itself requires separate CI confirmation. No offline-only development ZIP or patch is included.

The *portable host-only* IPC4 boot state model added after that build has no
PCI, DSP, DMA, I2C, GPIO, WDF or hardware backend. Its mock events and
verification fields are simulated values, not evidence from the Chromebook.
The real boot flag remains disabled in the Windows projects.

## Tests that can be run now without the Chromebook

Run these from the repository root **before** compiling `.sys` binaries:

```powershell
python tests/test_preinstall_guards.py
python tests/test_windows_ci_v017.py
python tests/run_offline_suite.py --logs build-logs/offline
```

The complete offline suite requires a host C compiler (GCC or Clang), plus
Python. On a Windows build host lacking GCC/Clang, the static Python checks
still work; do not mistake a skipped native test for a pass. Use GitHub Actions
for the full portable suite and WDK Debug/Release builds. Do **not** run the
source-only guard tests after generating `.sys` files; start with a clean
repository or remove only generated build outputs first.

The additional boot tests cover: evidence flags, wrong-board detection,
unknown/duplicate/stale FW_READY notifications, timeouts, fault latching,
restart and D3 invalidation, the production-locked path, and compile-time
rejection of mock code in a kernel build. The cross-component test connects
an in-memory boot-ready event with a fake SSP1 mute/amplifier lifecycle; it
verifies that no mock speaker path is unmuted before simulated readiness.
They do **not** send authentic IPC4 packets or model the actual DSP registers.

## Additional read-only Pujjo inventory

On the physical Chromebook running Windows, the script
`scripts/ReadOnly-PujjoIpc4Evidence.ps1` can list PnP device identities,
parent/location paths and driver status without installation or activation.
It must **not** be treated as confirmation of actual `_CRS` resources,
firmware trust or safe amplifier wiring. The ZimaOS Windows VM cannot
provide the Chromebook's PnP or ACPI evidence.

## Blocking work before a real driver package can be considered

- [ ] Obtain **read-only, on-device** PnP, ACPI `_CRS`, resource-hub, power
      and codec identity evidence for this exact Pujjo rev3 and firmware.
- [ ] Establish the actual IPC version, authenticated SOF image compatibility,
      manifest and topology data and firmware provenance. A filename or parser
      acceptance alone is not sufficient.
- [ ] Implement and review a real, WDK-supported DSP initialization and IPC
      transport with IRQL/DMA/interrupt and timeout handling, independent
      concurrency tests, and rollback for failed D0/D3 transitions.
- [ ] Implement and validate Windows SSP1 speaker path and SSP0 headset/mic
      endpoint topology. Host simulation cannot prove routing or timing.
- [ ] Confirm actual board-specific GPIO/SDB polarity, audio gain limits,
      speaker calibration and fault recovery **before** driving an amplifier.
- [ ] Audit and validate separate RT5682S and RT1019P resource ownership and
      teardown on surprise removal. Never infer pin numbers from raw ACPI.
- [ ] Establish actual Windows PnP device setup, signed drivers, package
      metadata/INF/extension relationships and reversible uninstall/rollback.
- [ ] Review on real Pujjo hardware under a controlled diagnostic plan;
      demonstrate safe behavior under failures and suspend/resume before
      offering an installer.

**Do not rename unsigned `.sys` outputs to an installer or package them as an
EXE.** A double-click installer is just a deployment wrapper, not a substitute
for the requirements above. Never disable Windows driver signing/integrity
controls as a shortcut.

## Hardware-free development completed after the original VM build

- Source-only IPC4 framing, transaction-state and FW_READY gating; these are
  not a verified DSP doorbell or ROM protocol implementation.
- NHLT parser bounds/checksum tests; no fabricated SoundWire device fallback.
- DMA segmentation/PCM geometry and negative-path tests; no physical DMA
  engine execution or IOMMU/MDL ownership validation.
- Fail-closed reply ownership, MMIO range checks and timeout handling;
  no verified interrupt ownership/ack ordering or PnP race resolution.
- Read-only PnP PCI/HDA resource discovery; premature PCI BME/MSE,
  postprocessing/PIE and HDA reset writes removed from discovery.
- MMIO BAR rollback when PrepareHardware fails; source-only checks do
  not exercise all WDF failure/power-state interleavings.
- WDK-linked sources and portable test suites, both Debug and Release CI.

## Actual remaining gate

Some remaining work **cannot** be completed or certified offline. Before
any installable driver can honestly be claimed, the exact Pujjo rev3 needs
read-only firmware/PnP/ACPI resource evidence, authenticated firmware and
topology compatibility, a validated ROM/IPC4/DMA/interrupt and power-state
backend, SSP/headset/microphone routing, codec/amp configuration, Windows
audio endpoint registration, signing and reversible installation. These
steps depend on measured hardware behavior and may require further source
changes after initial read-only diagnostics. An installer must not be
created merely because the regression suite is green.

## Preparing a candidate package

The Windows build produces an unsigned DSP driver, two independent peripheral
probe drivers and a static library. The library is a build dependency. Each
peripheral probe needs its own ACPI match and translated Resource Hub connection.
The DSP must not claim either peripheral's resources. Keep the USB and SoundWire
extension templates outside the Pujjo candidate pending matching device evidence.

Before creating a signed test package:

1. Export the device tree, hardware and compatible IDs, subsystem identifiers,
   translated resources and current driver-store package names from the target.
   Record the OS build and firmware version alongside the inventory.
2. Identify the approved ADL-N IPC4 image, its provenance and digest, and
   compare the module UUIDs, resource records, SSP formats and DMIC geometry.
3. Review each INF match and service against the inventory. Validate the
   staged INF files with the matching WDK InfVerif. Generate catalogs from
   the final files, sign through the authorized signing process and verify
   each catalog and its member hashes before distribution.
4. Preserve the existing signed packages and prepare an independent recovery
   boot path. Rehearse removal and restoration on a disposable Windows image.
   Record the published INF names so rollback targets only the candidate.
5. On the Chromebook begin with read-only inventory and identity checks.
   Qualify interrupt shutdown and DMA stop before firmware execution.
   Qualify capture before playback. Speaker activation also requires confirmed
   SDB polarity, calibration behavior, mute sequencing and safe gain limits.
