# Pujjo SOF — Preinstallation readiness and evidence (development, NOT an installer)

## Current status

**Not installable. Not hardware verified. No working speaker or microphone audio.**

The `main` revision `d990314c` passed Microsoft WDK Debug and Release compilation
on GitHub Actions run #10 and separately passed the Windows 11 VM build checks
(reported in user-provided `RESULTS.txt`). This proves only that the source
builds and that the unsigned intermediate artifacts are present. It does not
establish functional device initialization, a valid installable driver package,
or real SOF firmware boot.

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
