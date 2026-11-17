## v0.0.7 read-only device mapping

See [README_ACPI_DEVICE_MAP.md](README_ACPI_DEVICE_MAP.md). No hardware writes or installable driver.

# Pujjo / Pujjo1e Intel SOF Windows driver — source-only prototype v0.0.6

**NOT A WORKING AUDIO DRIVER YET. Do not install on your Chromebook.**

> **Latest development update (v0.0.6):** The mock speaker initializer is now explicitly SSP1 + RT1019P, and the separate SSP0/RT5682S clock planner does not write hardware. See [README_SPEAKER_HEADSET_SEPARATION.md](README_SPEAKER_HEADSET_SEPARATION.md). This remains **NOT INSTALLABLE**.



This is a **clean-source fork of `thesofproject/sof-windows-drv`** (BSD-3-Clause), scoped to Google **Pujjo** and **Pujjo1e** baseboards with the Intel Alder Lake-N PCI audio controller `8086:54C8` and the observed audio devices `ACPI\RTL5682` (codec) and `ACPI\RTL1019` (speaker amplifier).

This project is **independent of CoolStar activation**. It neither includes nor modifies CoolStar binaries. The separately supplied CoolStar archive contains compiled executables/DLLs and an installer, not editable C/C++ driver source. Only its README's board list and the user's own device IDs informed the target profile. Preserve original upstream LICENSE attribution.

## Actual source changes

| File | Change |
|---|---|
| `src/driver/sof_dsp.inx` | Matches only `PCI\VEN_8086&DEV_54C8&SUBSYS_00000000`; all other Intel/AMD device IDs removed from this fork's install template. This SUBSYS ID is **not unique to Pujjo**. |
| `src/driver/PujjoBoardId.[ch]` | Defensive, portable SMBIOS type-2 baseboard parser; permits only manufacturer `Google` with product `Pujjo` or `Pujjo1e`. |
| `src/driver/PujjoBoardGuard.[ch]` | Windows kernel-side SMBIOS gate using documented `AuxKlibGetSystemFirmwareTable('RSMB')`, fail-closed on errors. |
| `src/driver/Device.c` | Checks board **before PCI register writes**, rejects any PCI vendor/device except `8086:54C8`, removes dangerous fallback identification as a different Intel chipset, skips unverified SSP2 pipeline self-tests. |
| `src/driver/WaveRt.c` | SSP audio endpoint explicitly fails with `STATUS_NOT_SUPPORTED` until port and codec initialization are implemented; no misleading zero-data success. |
| `sof_dsp.vcxproj` | Adds Pujjo C files and WDK `Aux_Klib.lib` link dependency. |
| `scripts/read_only_pujjo_check.ps1` | Displays board, PCI/ACPI codec, firmware IDs without installing or changing anything. |
| `scripts/install_driver.ps1` | Hard-stops installation while speaker implementation is unfinished, including when invoked through `install.cmd`. |
| `tests/test_pujjo_boardid.c`, `tests/run_pujjo_tests.py` | 15 portable SMBIOS tests plus source-scope/safety assertions. |

## Verification performed here

- **PASS**: 15 compiled C board-ID tests (accept Pujjo/Pujjo1e, reject others and malformed data).
- **PASS**: source assertions ensuring INF restriction, runtime checks and skipped SSP2 audio experiments.
- **PASS**: upstream integrity checker for 48 unchanged Linux-derived source files.
- **NOT DONE**: Windows MSBuild/WDK compile — unavailable in this Linux runtime.
- **NOT DONE**: driver load, Windows validation or hardware sound test — no Pujjo test hardware.
- **NO** installable `.sys` or signed driver included.

To run software-only tests with a C compiler and Python 3:

```sh
python tests/run_pujjo_tests.py
python scripts/verify_upstream_integrity.py
```

On Windows you may run the **read-only** preflight in PowerShell from the project root:

```powershell
.\scripts\read_only_pujjo_check.ps1
```

Do **not** follow the unmodified upstream README's turnkey-install steps for this experimental fork. The installer has been blocked on purpose.

## Critical missing work before internal speakers are possible

1. Confirm the actual SSP port(s), bit clocks, TDM slots, and firmware topology for **Pujjo/Pujjo1e** from independently licensed hardware documentation or read-only ACPI/NHLT data.
2. Verify compatible, properly licensed **Alder Lake-N SOF firmware** and the IPC protocol. The uploaded open-source source archive does not contain ready firmware.
3. Develop or integrate independently licensed, **Windows-compatible RT5682 codec and RT1019 amplifier** initialization and power sequencing. Merely matching ACPI IDs does not initialize those chips.
4. Add safe speaker mute/volume control, jack detection, robust shutdown, and resume handling.
5. Complete Windows WDK build/signing in a developer environment; test first on a fully recoverable **separate** system, with speaker safety safeguards.

**Important:** even a successful MSBuild does not imply audio support. The SSP endpoint is intentionally disabled until the missing audio path is implemented. Never patch or repurpose the proprietary CoolStar license code as a substitute for this work.

## Linux topology inspection (v0.0.2)

See [README_LINUX_TOPOLOGY.md](README_LINUX_TOPOLOGY.md) for the reference graph: candidate speakers `SSP1`, headset `SSP0`, and digital BT link `SSP2`; `RTL5682` / `RTL1019` map to Linux `rt5682s` / `rt1019p`. The source-only fork has not enabled playback. The Linux binary topology is **not included**.

## Linux kernel source verification (v0.0.3)

The user supplied a 317 MB official Linux kernel source ZIP. See
`README_KERNEL_PORTING.md` for verified codec and SOF machine-driver mappings
and the licensing boundary. Run `python tests/verify_kernel_reference.py
/path/to/linux-master.zip` to re-check the mappings without touching hardware.
This change adds **reference documentation and validation only**; it does
not enable speaker playback.

## Audio initialization coordinator (v0.0.4)

The experimental portable C module `src/driver/experimental/PujjoAudioInit.[ch]`
implements a **fail-closed staged codec/amp initialization coordinator** with
simulated tests; see `README_CODEC_INITIALIZATION.md`. It intentionally does
**not** include Windows I2C, GPIO, ACPI resource access, driver integration or
audio playback. `scripts/read_only_codec_preflight.ps1` is safe PnP inspection.
Run `python tests/run_codec_init_tests.py` for locked-mode and mock-mode tests.
The original BSD and independent Linux GPL licensing obligations are documented
in `README_CODEC_INITIALIZATION.md`.

## Linux topology container inspection (v0.0.5)

See `README_TOPOLOGY_VALIDATION.md`. Added compiled portable ALSA topology
header/route inspection and tests against the *actual* user-supplied
Alder Lake RT1019/RT5682 `.tplg`. 31 structurally valid sections and the
candidate SSP1, SSP0 and SSP2 routes were found; malformed/truncated/wrong-route
files are rejected. This evidence does **not** make the Windows speaker path
functional or enable any kernel/hardware access.


## v0.0.9 update

Historical v0.0.9 note: that version used `BUILD.bat`; it has been removed in v0.0.10. The KMDF companion projects are still resource probes only.

## v0.0.10 packaging note

The Windows build entrypoint is now `Build_Pujjo.pyw` or `sof_windows_driver.sln` (four projects). `BUILD.bat` has been removed. There is an unverified, optional cloud CI workflow in `.github/workflows/compile-windows.yml`. No runtime/audio functionality was added. See `START_HERE.md`.
