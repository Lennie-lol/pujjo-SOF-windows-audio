# Review of what is established (14 November 2026)

**Development-only v0.0.10. No completed driver, validated WDK build, driver installation, firmware initialization, or speaker playback.**

## Distilled hardware evidence

- Board restriction: Google Pujjo / Pujjo1e, Intel PCI audio controller `8086:54C8`.
- Static raw ACPI evidence from Pujjo firmware: `RTL5682` I2C3 codec at 7-bit address `0x1A`, 400 kHz, GPIO interrupt 87.
- `RTL1019` is a GPIO-controlled amplifier resource on pin 75; **no amplifier I2C resource** was advertised. ACPI GPIO numbers are not Windows Resource Hub connection identifiers.
- SSP1 speaker / SSP0 headset mapping is a **candidate based on Linux references**, not a confirmed Windows-compatible SOF topology.
- No raw user ACPI `.dat` tables or proprietary CoolStar driver binaries are included in the package.

## What is genuinely in this package

- Original Windows SOF source fork plus board, PCI and ACPI safety guards.
- New source-only KMDF RT5682 and RT1019 resource-discovery projects wired into a four-project Visual Studio `.sln`.
- Development GUI `Build_Pujjo.pyw` (software checks and attempted WDK compilation), replacing `BUILD.bat`.
- **Unverified** optional GitHub Actions `windows-2022` hosted build workflow; requires a GitHub repo and may need WDK/VSIX setup fixes.
- No INF/CAT driver package, signing, installer, or compiled `.sys` file. No live I2C/GPIO, amplifier power, DSP firmware loading or speaker audio implementation.

## Tests actually run in this environment

Passed on Linux (host-side and static only):
- `tests/test_kmdf_security.py`
- `tests/test_acpi_parser.py` (9 checks)
- `tests/test_sof_zephyr_log.py` (7 passed, 1 skipped because its optional external ELF file is unavailable)
- `tests/verify_linux_topology.py` (local mapping only, external Linux ZIP not supplied)
- `scripts/verify_upstream_integrity.py` (48 tracked files verified)
- `tests/run_acpi_resource_tests.py`
- `tests/run_codec_init_tests.py`
- `tests/run_device_map_tests.py`
- `tests/run_headset_clock_tests.py`
- `tests/run_pujjo_tests.py`

Not passed or not performed: WDK/MSBuild compilation, driver loading, runtime device-resource enumeration, Pujjo DSP boot, I2C/GPIO hardware control, speaker testing, HLK and signing. The unrelated upstream `tests/run_regression_suite.py` requires an external DUT automation dependency and did not run successfully here; `tests/run_topology_evidence_tests.py` requests a separate Linux topology ZIP. Neither is part of the GUI's five Python checks.

## Constraints and next realistic milestone

A cloud build can avoid using the Chromebook's limited internal storage for VS/WDK, but it **cannot substitute** for hardware testing or establish a safe driver. If the hosted build succeeds, it only demonstrates source compilation. Proper resource assignment, firmware and protocol selection, power management, recovery, signing and install safety must be verified before trying any kernel driver on a daily-use machine. This package is for inspection and future development, **not installation**.

See `START_HERE.md` for the build choices. Supported VS 2022 WDK family: https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads ; MSBuild driver builds: https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-a-driver .
