> **Current release: v0.0.14 — SOURCE ONLY, NOT INSTALLABLE.** See [CODEX_RESULTS.md](CODEX_RESULTS.md), [CHANGES.md](CHANGES.md) and [REMAINING_BLOCKERS.md](REMAINING_BLOCKERS.md). Run `python tests/run_offline_suite.py` for the full offline allowlist. Windows WDK compilation remains unverified.

> The material below is retained release history; earlier version numbers and test counts describe those earlier packages, not v0.0.14.

# Pujjo/Pujjo1e driver development fork v0.0.12

**Status: source prototype; NOT a working Windows audio driver.**

This fork now adds two *real Windows KMDF project targets* for **ACPI resource discovery**:

- `pujjo_rt5682_probe.vcxproj` — enumerates the Windows-assigned I2C resource for ACPI RTL5682.
- `pujjo_rt1019_probe.vcxproj` — enumerates the Windows-assigned GPIO I/O resource for ACPI RTL1019.
- `src/driver/peripheral/PujjoResourcePolicy.[ch]` — bounded, mock-testable resource validation.
- `src/driver/peripheral/PujjoPeripheralKmdf.c` — KMDF DriverEntry, DeviceAdd, PrepareHardware, ReleaseHardware.
- Original `sof_windows_driver.sln` — retained open-source SOF PCI/DSP driver foundation.

The new code rejects unexpected resource types, resource duplicates, and non-Pujjo/Pujjo1e SMBIOS boards. It **never opens** a Resource Hub connection and **never sends I2C or GPIO transactions**. It does not include PnP installation INFs or any installer scripts. The Realtek controls, SOF firmware boot on Pujjo, SSP1 speaker routing, gain limits, and playback remain unfinished.

## Compile on Windows 11

Install **Visual Studio 2022 with Desktop Development with C++**, **Windows SDK 10.0.26100.0**, **Windows Driver Kit 10.0.26100.0**, and **Python 3** (accessible as `py -3`). Then double-click **`Build_Pujjo.pyw`** to open the source-only development GUI, or open **`sof_windows_driver.sln`** in VS 2022 and select Build Solution. See `START_HERE.md` for the optional experimental GitHub Actions cloud build path.

The GUI **Run software checks** button now runs the explicit offline allowlist, including host C tests (a host C compiler is required). Hardware and external-fixture suites are excluded and reported separately. The **Compile all projects (x64)** button checks the WDK/SDK headers and MSBuild, then attempts to build the four projects in `sof_windows_driver.sln`. The build currently has not been verified on Windows and may fail. **No driver is installed, no test-signing mode is enabled, and no certificate is created.**

### Tests

- C policy tests: portable, compiled and executed here with Clang/GCC; also run via MSVC via the optional developer build GUI.
- Static source gates: run via `python tests/test_kmdf_security.py`.
- WDK kernel compilation: **must be verified on a Windows machine with matching WDK**. These artifacts were prepared outside Windows; no successful MSBuild compile is claimed.
- CodeQL Static Tools Logo Test, Windows Driver Verifier, HLK, real Pujjo speaker playback, and physical security/safety verification: **NOT run/passed**. Microsoft has retired Static Driver Verifier from the WDK 26100 generation; CodeQL is the recommended modern analysis path.

## Technical design

Firmware evidence from Pujjo rev3 (user-collected ACPI): `PCI0.I2C3.RT58` codec, I2C `0x1a` 400kHz, GPIO interrupt 87, and `PCI0.I2C3.ALCP` RT1019P amplifier, GPIO `sdb-gpios` pin 75. These are **firmware descriptions**, not validated Windows translated-resource identifiers or commands to power the hardware. Windows drivers must use the PnP-provided **Resource Hub connection IDs**, rather than hard-code these pin numbers or open I2C controller registers.

Microsoft documents `EvtDevicePrepareHardware`, `WdfCmResourceListGetDescriptor`, and Resource Hub connections for SPB and GPIO peripherals. A working driver will later need separate I2C read/write and GPIO control paths, with verified speaker-gain limiting and safe power-off sequence, and a correctly configured Intel SOF DSP topology.

**Do not manually force-install** these .sys files. Windows kernel-mode driver installation requires properly signed, validated INF/CAT packages; none are supplied. Building is a developer activity, not audio enablement.

## Sources

- Microsoft: [Building a driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-a-driver)
- Microsoft: [Resource Hub connections for SPB](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/connection-ids-for-spb-connected-peripheral-devices)
- Microsoft: [GPIO driver support](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-driver-support-overview)
- Microsoft: [WDF hardware resource handling](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/finding-and-mapping-hardware-resources)
- Microsoft: [Static Driver Verifier retirement and CodeQL guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/static-driver-verifier)
- Linux kernel codec references: `sound/soc/codecs/rt5682s.c`, `sound/soc/codecs/rt1015p.c`, `sound/soc/intel/boards/sof_rt5682.c` (GPL-2.0-only; not copied)
- Windows SOF base project: https://github.com/thesofproject/sof-windows-drv

The included source retains upstream notices. No proprietary CoolStar binary is included, and no license activation mechanism is modified.
