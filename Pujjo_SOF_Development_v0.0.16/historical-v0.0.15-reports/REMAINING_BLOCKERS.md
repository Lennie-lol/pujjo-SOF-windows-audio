# Remaining blockers — v0.0.14

**This project cannot yet be installed or used for audio.**

## Code work

- Finish validated ADL-N IPC4 firmware handling: real signed-image fixtures, complete
  module-table/manifest checks and verified Windows DSP graph/SSP mapping. `sof-adl-n.ri`
  and Linux `.tplg` filenames alone prove none of these. Parser is metadata-only.
- Review inherited contiguous DMA fallback: physical addresses are not necessarily
  device-visible IOMMU addresses. Prove DMA quiescence before buffer release and every
  power/timeout failure path. Boot is blocked, but enabling a macro is not a fix.
- Implement full RT5682S power/reset/calibration, PLL/DAI, jack/microphone paths and
  codec/DSP coordination. Identity reads and a mock register update are not initialization.
- Design serialized, power-aware Windows transport ownership/cancellation/teardown.
  Current helpers assume caller serialization; they must not race with target close.
- Integrate a reviewed amplifier lifecycle only after polarity, mute and gain validation.
  The portable model is not linked into amplifier PnP; GPIO helper remains uncalled.
- Replace inherited platform assumptions for PortCls endpoints, IPC, NHLT, suspend and
  power recovery with target-specific evidence. A disabled or successful software model
  is not a functioning endpoint.
- Provide explicit firmware-layout policy for any future non-cAVS2.5 target. Current
  parser behavior is scoped to this fork's ADL-N candidate; other offsets are unsupported.

## Windows build verification

- Run the provided workflow on a disposable Windows worker; confirm VS WDK component,
  SDK/WDK installation, Release/Debug MSBuild, linker dependencies and WDF API signatures.
  This environment could not launch a Windows compiler; no successful build is claimed.
- Run MSVC analysis / CodeQL and appropriate driver checks. Host GCC, sanitizers and
  extracted-function mocks do not validate IRQL, SAL, WDF ownership or Windows ABI.
- Check CI provisioning against the current hosted runner image and retain failed logs.
- Execute the complete inherited Windows host test program after adding a supported test
  build target. Its corrected manifest fixture is currently source-only.

## Requires controlled hardware and a recovery plan

- Independently confirm Pujjo/Pujjo1e identity, PCI 8086:54C8, RTL5682/RTL1019 resources,
  exclusive Windows Resource Hub ownership, GPIO grouping and electrical SDB polarity.
- Verify signed ADL-N firmware/IPC boot, SSP clocks/routing and a valid Windows topology.
- Confirm RT5682S rails, reset/calibration and analog behavior; confirm speaker-safe mute,
  volume/gain caps and amplifier behavior, starting with a recoverable dedicated device.
- Exercise repeated D0/D3/S0ix, power loss, failed transactions, removal and rollback;
  run Driver Verifier/HLK as appropriate.
- Only after these prerequisites: prepare reviewed reversible packaging/signing and
  recovery procedures and separately authorize any controlled test. No such package or
  hardware authorization is provided by this source archive. Do not enable the gates
  or install these prototypes on the school Chromebook.

Missing external fixtures: real Zephyr ELF, user topology archive, full Linux reference
archive. LeakSanitizer requires a host without the observed ptrace restriction. Clang
and MSVC were unavailable locally. All skips are documented rather than counted as passes.

## v0.0.15 clarification
The new `PujjoBringupCoordinator` is a verified **software model only**. It does not remove any blockers listed above, and `PJ_BRINGUP_TEST_CALLBACKS` is explicitly forbidden in the Windows kernel. Its readiness inputs are declarations requiring independent real-world verification, not a method of proving hardware compatibility. See `README_BRINGUP_v0.0.15.md` for the full remaining implementation matrix.
