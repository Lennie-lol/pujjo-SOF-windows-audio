> **Current release: v0.0.14 — SOURCE ONLY, NOT INSTALLABLE.** See [CODEX_RESULTS.md](CODEX_RESULTS.md), [CHANGES.md](CHANGES.md) and [REMAINING_BLOCKERS.md](REMAINING_BLOCKERS.md). Run `python tests/run_offline_suite.py` for the full offline allowlist. Windows WDK compilation remains unverified.

> The material below is retained release history; earlier version numbers and test counts describe those earlier packages, not v0.0.14.

# Intel SOF Alder Lake-N DSP / firmware research — Pujjo v0.0.13

**NOT INSTALLABLE. No firmware blob, bootable driver, INF, CAT or SYS in this package.**

## Verified research (not hardware proof)
- User-provided PCI controller: Intel `8086:54C8` (Alder Lake-N N100). Board guard covers `Google Pujjo` / `Pujjo1e`.
- Official SOF project references name the **ADL-N** firmware file `sof-adl-n.ri`, *not* `sof-tgl.ri`. Linux software carries distinct IPC3 and IPC4 images (same filename in different directories).
- Example SOF-bin **source repository** paths (do not treat these as Windows installation paths):
  - IPC3: `sof/intel-signed/sof-adl-n.ri`
  - IPC4: `sof-ipc4/adl-n/intel-signed/sof-adl-n.ri`
- Inherited Windows SOF loader previously chose default `tgl` for all cAVS 2.5 devices including `8086:54C8`. The wrong binary could be selected and the parser assumes a v4-style manifest. The source now blocks all firmware boot by default and explicitly refuses ADL-N without reviewed IPC4 configuration *even if* the first boot lock were removed.
- The Linux `sof-adl-rt1019-rt5682.tplg` (or user-provided `sof-adl-rt1019-rt5682.tplg` graph) is Linux topology, **not a compatible Windows IPC4 topology**. Matching filenames does not prove compatible DSP graph, IPC or real audio.
- The Windows loader's current `SofParseFirmwareImage` checks `$AE1` v4 manifest magic, but has not been validated against a real, officially signed ADL-N IPC4 firmware. Do not assume a successful parse proves cryptographic signature validation.

## Changes in this release
1. `src/driver/experimental/PujjoSofFirmwarePolicy.[ch]` -- host-testable, strict vendor+device and explicit IPC3/IPC4 source filename policy. No DMA/MMIO and no Windows dependencies.
2. `src/driver/Device.c`: `SofEvtDeviceD0Entry` returns `STATUS_NOT_SUPPORTED` in the default build before any power/IPC/MMIO initialization. This prevents a misleading apparent boot if someone compiles the source-only code.
3. `src/driver/Loader.c`: `SofStartFirmwareBoot` returns `STATUS_NOT_SUPPORTED` by default. An *additional* IPC4-reviewed compile-time flag protects experimental ADL-N path selection. **Neither enable macro is defined in the Visual Studio project or build workflow.** This is NOT a ready-to-activate hardware path.
4. `tests/run_pujjo_sof_fw_tests.py` and `tests/test_pujjo_sof_fw_static.py` -- mock-free portable policy tests, default hardware boot block checks, and no installable artifacts.
5. `scripts/ReadOnly-PujjoSofPreflight.ps1` -- optional **read-only** native Windows inventory: board product, matching PnP IDs and presence of expected local file paths. No admin permission, WDK, installation or device commands.
6. The existing Tkinter build UI and untested GitHub Actions workflow now include the new tests.

## What cannot be established offline
- Whether the target N100 DSP boots a particular signed image, how the boot ROM/manifest handles it, and which IPC version the running Windows driver should use.
- Proper DSP clock/power/ROM handshakes, firmware signatures, SOF IPC4 hello readiness, kernel-mode resource constraints, and system power recovery.
- A compatible, legally redistributable firmware package for Windows, and the verified speaker topology, safety gain/mute, and RT5682S+RT1019P integration.
- Successful Windows WDK build or actual speaker playback. Only actual controlled Windows tests can verify those.

## Optional Chromebook check without compiling anything
Run **only the read-only preflight** in regular (non-admin) Windows PowerShell:

```powershell
& '.\scripts\ReadOnly-PujjoSofPreflight.ps1'
```

This may be blocked by your machine's execution policy. Do not disable system security or install drivers just to run it; you can inspect the script manually instead. It reports identifiers and file presence, **not firmware boot or playback**. It does not transmit information.

## Official references
- SOF developers' ADL-N firmware filename confirmation: https://github.com/orgs/thesofproject/discussions/9079
- SOF firmware tree and IPC3/IPC4 variants: https://github.com/thesofproject/sof-bin
- Windows upstream code foundation: https://github.com/thesofproject/sof-windows-drv
- Linux Alder Lake-N platform: https://github.com/torvalds/linux/blob/master/sound/soc/sof/intel/pci-tgl.c

**Important:** No firmware is downloaded or redistributed by this package, and none of the modified hardware boot routines has been run or Windows-compiled.
