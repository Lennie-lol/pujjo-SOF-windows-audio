# Changes in v0.0.16 — 2026-11-17

- Hardened the production IPC4 metadata parser: $AM1/$AME identifiers, nonempty bounded module tables, overflow-resistant sizes and configuration ranges. Added malformed-image cases and 20,000 deterministic fuzz inputs. Parsed a real official ADL-N 2.12 image; parsing is not signature authentication.
- Removed the physical-address DMA fallback. Production allocation now requires a WDF DMA enabler and uses the common buffer's device logical address; failure leaves cleared output. Added tests against the actual allocator function.
- Added persistent device-parented wait locks and explicit powered state to both Windows Resource Hub transports. Serialized open/I/O/power/close, prohibited I/O outside D0, retained locks through resource rebalance, and cleaned failed targets. No guessed connection IDs or GPIO polarity.
- Updated the optional codec probe lifecycle to initialize its transport once, use passive callbacks, and invalidate stale resource evidence. Real probe I/O remains opt-in; GPIO remains disconnected.
- Added a pure RT5682S PLL2 coefficient planner for measured 19.2/24/48 MHz MCLK to 24.576 MHz. It returns five masked register updates, performs no I/O and does not imply PLL lock, calibration or correct board wiring.
- Fixed coordinator fault-stop handling to retry mute/disable without clearing the fault latch. Documented its caller-serialized contract. It remains a host-only model.
- Corrected the main DSP project from desktop Application/v143 to KMDF Driver/WDK toolset, KMDF 1.33 build properties, inherited kit paths/default driver libraries, and standard framework startup. Removed the manual security-cookie/FxDriverEntry wrapper.
- Split portable CI checks from the independent Debug/Release Windows jobs so a portable failure does not prevent a Windows build attempt. CI retains logs and labels any future products NOT-INSTALLABLE-NOT-HARDWARE-VERIFIED.
- Added full-source Windows transport tests using WDF doubles, including deterministic concurrent close/read, 1,000 power cycles, short transfers, failed opens, failed sends and reopen. Expanded strict, sanitizer and GCC analyzer matrix.

No hardware activation or security bypass was added. Existing licenses and upstream source remain preserved. Current results supersede inherited audit reports; historical results are not validation of this release.

# v0.0.14 — 14 November 2026

Software audit and regression fixes; see [CHANGES.md](CHANGES.md) and [CODEX_RESULTS.md](CODEX_RESULTS.md). Source only; Windows build and all hardware remain unverified.

## v0.0.13 (2026-11-14)

- Established correct Alder Lake-N `sof-adl-n.ri` candidate filename and IPC3/IPC4 repository split.
- Added portable fail-closed SOF firmware selection policy with host tests.
- Explicitly blocks DSP boot and DMA in the default Windows driver source.
- Added non-privileged read-only Chromebook preflight script (PnP/board/file presence only).
- Kept Windows compile, DSP boot, signature/IPC compatibility, speakers UNVERIFIED.

## v0.0.12 (2026-11-14) – RT1019P source development
- Added fail-closed, portable RT1019P/RTL1019 amplifier state machine with 300 ms calibration model and explicit mute, shutdown, suspend/resume and fault handling.
- Added Windows KMDF GPIO Resource Hub transport source (`WdfIoTargetOpen`, `IOCTL_GPIO_WRITE_PINS`), included in amp probe project for compilation only; not called in PnP or sound playback.
- Added host C mock and locked tests, static source safety checks, GUI and CI test hooks, and comprehensive status notes.
- Never installed, loaded, or tested the Windows kernel components; hardware writes remain unavailable from the PnP probe.

## v0.0.10 (2026-11-14)

- Moved from a batch launcher to a small Tkinter source-development GUI.
- Added two KMDF resource-probe projects to `sof_windows_driver.sln`.
- Added an optional GitHub Actions hosted Windows build workflow (not yet run).
- Updated static safety checks to cover the GUI, rather than a batch file.
- No installable driver, ACPI hardware activation, or playback added.

# v0.0.9 (November 13, 2026)

- Added two Windows KMDF resource-discovery driver projects for RTL5682 and RTL1019 PnP nodes.
- Bounded, pure-C resource validation (I2C vs GPIO, duplicate Resource Hub IDs, unexpected classes).
- Fail-closed SMBIOS board guard and exact Realtek hardware ID validation.
- Hardware writes, firmware boot, and speaker audio remain disabled.
- Added one root `BUILD.bat` to check/test/build on Windows with VS 2022 + WDK.
- Removed install/signing scripts and installation CMDs so this ZIP cannot accidentally deploy untested kernel drivers.
- Added `README_BUILD_FIRST.md` with Microsoft documentation and explicit unverified WDK/HLK/SDV status.

# Pujjo fork development history

## v0.0.7 (source-only, non-installable)
- Confirmed from actual Pujjo rev3 Windows output: RTL5682 lives under ACPI I2C3.RT58, RTL1019 under I2C3.ALCP, both have PnP parent Intel PCI DEV_54EB.
- Added a portable C device-map validator, 10 host-side test cases, and optional structured read-only PowerShell inventory export.
- Kept real ACPI/I2C/GPIO/SSP initialization disabled; addresses, amp enable polarity, and speaker volume remain unverified.
- This release is not an installable or working sound driver.

# Pujjo fork development history

## v0.0.6 (source-only, non-installable)
- Corrected speaker startup state machine: SSP1 speaker DSP mute + RTL1019/RT1015P-family amplifier power, NOT RT5682S headset codec.
- Introduced separate RT5682S/SSP0 sample format and BCLK/SYSCLK arithmetic planner; unknown input clocks remain unknown.
- Added speaker gain-limiter readiness flag to the test coordinator (no limiter is implemented, so real-hardware activation remains forbidden).
- Added read-only Windows ACPI/PnP inventory script.
- Revalidated mocked startup/failure, board identification, Linux topology markers, and Linux kernel reference mappings.
- **Not implemented:** real ACPI/WDF bindings, GPIO/I2C, PortCls speaker endpoint, SOF firmware, codec writes, gain limits or WDK build.

## v0.0.5
- Added strict ALSA topology container and route-marker validation.

## v0.0.4
- Added initial portable mock codec/amp coordinator (superseded in v0.0.6).

## v0.0.3 and earlier
- Scoped board/PCI support to Google Pujjo/Pujjo1e; identified Linux audio references.

## v0.0.8 — Raw ACPI resource extraction

- Analyzed the device's DSDT/SSDT without running AML methods.
- Confirmed RT5682 codec 7-bit I²C address `0x1A` (400 kHz), GPIO interrupt 87.
- Confirmed RTL1019 amplifier GPIO output 75 (`sdb-gpios`) and **no I²C resource** in its `_CRS`.
- Added offline parser and fail-closed C evidence validator with tests. None is linked into the Windows kernel driver.
- No new speaker activation capability; driver install remains disabled.

## v0.0.15 (2026-11-17) — integration preflight model

- Added portable, mock-only Pujjo SSP1 audio bring-up coordinator combining existing board, firmware, amplifier and speaker evidence checks.
- Modeled conservative ordering, first-enable calibration delay, hard-failure rollback, latched faults and suspend/resume requiring fresh readiness evidence.
- Added locked/mock compiler test, multiple failure-injection cases, compile-time prohibition of enabling mock callbacks in the Windows kernel, and a static check that the hardware firmware boot locks remain present.
- Added hardware-free suites to the existing offline suite. No real hardware endpoints, Windows firmware boot, WDK compilation, device installation, signing or speaker playback added.
