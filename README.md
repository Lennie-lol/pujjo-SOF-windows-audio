# Pujjo SOF Windows Audio Driver

**Experimental open-source Windows 11 audio driver development for Google Pujjo / Pujjo1e Chromebooks**  
**Build milestone (2026-11-17):** GitHub Actions #10 on `main` passed the WDK
Debug and Release builds plus portable regressions. Windows 11 VM Debug and
Release builds also passed. See [PREINSTALL_READINESS.md](PREINSTALL_READINESS.md)
for the remaining blockers. No version bump.

**Development update (2026-11-18):** GitHub Actions [run #69](https://github.com/Lennie-lol/pujjo-SOF-windows-audio/actions/runs/44368075960) passed WDK Debug/Release and portable regression jobs on development commit `4831d721`. All repository development branches are consolidated into `main` using the newest validated source, with older branch history preserved and documentation reconciled. This is not proof of functional audio; the consolidation commit requires its own validation. Offline-only development changes are excluded. See [PREINSTALL_READINESS.md](PREINSTALL_READINESS.md).

**Current source version:** `v0.0.17` · **Status:** source-only / development · **License:** see [`LICENSE`](LICENSE)

> [!CAUTION]
> **This project does not yet provide working Windows audio.** The WDK Debug/Release source-only builds now pass, but the unsigned output is **not an installable driver**. It has not been installed on the target device, booted Intel SOF firmware, or played audio through the internal speakers. There is **no validated installable driver package**. Live DSP boot, codec writes, and amplifier activation remain disabled by default. **Do not install or force-load the prototype, bypass its guards, or use the preserved upstream installation instructions.**

This README consolidates the important Pujjo-specific information from the previous 14 `README*.md` files. Older release notes are historical, not proof of what v0.0.17 can do. The inherited SOF Windows source also contains generic HDA, SoundWire, USB, HDMI, AMD and other-platform code; **those upstream features and support claims are not verified for this Pujjo fork**.

## Contents

1. [Goal and current status](#goal-and-current-status)
2. [Target hardware and actual ACPI evidence](#target-hardware-and-actual-acpi-evidence)
3. [How the audio paths fit together](#how-the-audio-paths-fit-together)
4. [Implementation by subsystem](#implementation-by-subsystem)
5. [What changed in v0.0.17](#what-changed-in-v0017)
6. [What changed in v0.0.16](#what-changed-in-v0016)
7. [Source layout](#source-layout)
8. [Offline tests and current results](#offline-tests-and-current-results)
9. [Compiling on Windows or GitHub Actions](#compiling-on-windows-or-github-actions)
10. [Read-only hardware diagnostics](#read-only-hardware-diagnostics)
11. [Remaining work and safe development order](#remaining-work-and-safe-development-order)
12. [Licensing and upstream references](#licensing-and-upstream-references)

## Goal and current status

The goal is an independently developed Windows 11 x64 audio solution for an Intel Alder Lake-N Chromebook with its onboard Realtek codec and amplifier. It is **not** a CoolStar activation bypass and contains no proprietary CoolStar driver binaries or license modifications.

| Area | v0.0.17 status |
| --- | --- |
| Board/PCI identification | Defensive Pujjo/Pujjo1e checks implemented; offline-tested |
| Raw ACPI / Linux mapping | Read-only evidence inspected; Windows runtime resource validation still needed |
| RT5682S codec | Identity tests, guarded SPB transport source and offline PLL2 coefficient planning; no complete codec initialization |
| RTL1019 / RT1019P amplifier | GPIO Resource Hub transport source and simulated power/fault lifecycle; not connected to real playback |
| Intel SOF firmware | ADL-N selection and manifest metadata parser improved; no authenticated, working DSP boot |
| DMA / concurrency | Safer common-buffer policy and serialized transport lifecycle source; mock/host-tested only |
| SSP topology and Windows endpoint | Candidate Linux graph inspected; **no working Windows WaveRT render/capture pipeline** |
| Windows WDK build | GitHub Actions #10 on `main` passed both Debug and Release unsigned builds; Windows 11 VM independently passed both |
| Speaker / headset / microphone audio | **Not working and not hardware-tested** |

A successful host test or a future `.sys` compilation is **not** proof that hardware sound works.

## Target hardware and actual ACPI evidence

The following items come from the project's user-supplied Pujjo rev3 Windows inventory and offline inspection of its ACPI firmware tables; they describe hardware, **not permission to operate it directly**.

| Component | Device / path | Evidence |
| --- | --- | --- |
| Mainboard | Google `Pujjo` rev3; `Pujjo1e` is an intended, separately unverified family target | SMBIOS board guard accepts only Google Pujjo/Pujjo1e |
| CPU / SOF audio controller | Intel N100 Alder Lake-N; `PCI\VEN_8086&DEV_54C8` (`PCI0.HDAS`) | Actual Windows PnP ID; prototype INX narrowed to `SUBSYS_00000000`, **not uniquely identifying the board** |
| RT5682S headset codec | `ACPI\RTL5682` at `PCI0.I2C3.RT58` | Intel I²C parent `8086:54EB`; ACPI I²C address **0x1A**, **400 kHz** |
| Codec interrupt | `\_SB.PCI0.GPIO` | ACPI GPIO interrupt **pin 87**; `_DSD` mentions `irq-gpios` and `realtek,jd-src` |
| RT1019P-compatible amplifier | `ACPI\RTL1019` at `PCI0.I2C3.ALCP` | **GPIO output**, ACPI **pin 75**, `_DSD` property `sdb-gpios` |
| RTL1019 I²C connection | None in its inspected `_CRS` | The ACPI node being under `I2C3` **does not mean the amp is an I²C codec** |
| BIOS / firmware | coreboot / MrChromebox | Hardware inventory context |

**Important limits:** Raw ACPI GPIO numbers are not Windows translated Resource Hub connection IDs. Neither a raw pin number nor an I²C address should be hard-coded for real transfers. Polarity, clock sources, electrical behavior, IRQ trigger properties, mute and safe output level are **not** validated by this evidence. The `_CRS` descriptors were extracted without evaluating AML methods; this is not a live Windows resource test.

### Linux driver/source correspondences

| Hardware | Linux reference | Why it matters |
| --- | --- | --- |
| Intel ADL-N `8086:54C8` | `sound/soc/sof/intel/pci-tgl.c` | SOF platform/device family |
| `RTL5682` RT5682S | `sound/soc/codecs/rt5682s.c` | Headset codec, I²C, clocks, DAI, jack path |
| `RTL1019` RT1019P-compatible | **`sound/soc/codecs/rt1015p.c`** | GPIO-controlled shutdown/enable and first-power calibration |
| Machine/device matching | `sound/soc/intel/common/soc-acpi-intel-adl-match.c` | Codec combination and topology selection |
| Codec/amp link wiring | `sound/soc/intel/boards/sof_rt5682.c` | Linux ASoC machine/DAI setup |

**Do not confuse** the RTL1019 ACPI alias of the `rt1015p`-family amplifier driver with the unrelated Linux `rt1019.c` driver. The Linux sources are reference material under their own licenses and have not been copied wholesale into this Windows fork.

## How the audio paths fit together

The user-supplied Linux `chromebook-linux-audio` archive contains the reference file `sof-adl-rt1019-rt5682.tplg` (55,343 bytes). Offline inspection found **31 ALSA topology sections** and these candidate routes:

| Candidate path | Linux topology labels | Purpose |
| --- | --- | --- |
| **SSP1 OUT** | `PIPELINE.1.SSP1.OUT`, `SSP1-Codec`, `Speakers 0` | Built-in speaker output via the RTL1019 amplifier |
| **SSP0 OUT / IN** | `SSP0-Codec`, `Headset 1`, `PCM1P/PCM1C` | RT5682S headphone and headset microphone |
| **SSP2 OUT / IN** | `SSP2-BT`, `Bluetooth 7` | DSP digital Bluetooth link; **not** ordinary Windows Bluetooth headphone pairing |
| **DMIC0 / DMIC1 IN** | `PCM99C`, `PCM100C` | Candidate digital microphone paths |

These are **Linux topology candidates**, not proof of Pujjo's Windows SSP pinmux or a working Intel IPC4 pipeline. The Linux `.tplg` cannot be loaded as a drop-in Windows PortCls/WaveRT topology.

### Design separation

```text
Windows apps / Windows Audio Service / WASAPI
                       |
              PortCls / WaveRT       <-- NOT functional on Pujjo yet
                       |
         Intel SOF DSP + IPC + DMA  <-- boot, IPC4 and stream integration incomplete
                /             \
  SSP1 speaker candidate      SSP0 headset candidate
          |                         |
  RTL1019 / RT1019P amp      RTL5682 / RT5682S codec
  GPIO SDB + safe DSP gain   SPB I²C + clocks/PLL + jack/mic
```

The speaker amplifier and headset codec are **separate hardware paths**. Earlier mock code accidentally used the headset codec while preparing SSP1 speakers; that model was corrected beginning with v0.0.6.

### Illustrative clock and enable logic (not hardware settings)

For 48 kHz stereo audio, two 32-bit slots mathematically imply a **3.072 MHz BCLK**; two 24-bit slots imply **2.304 MHz**. A common proposed SYSCLK is **24.576 MHz**. These are planning values, **not measured Pujjo clock or slot settings**. The v0.0.16 RT5682S PLL2 planner supports candidate measured MCLK inputs of 19.2, 24 or 48 MHz; it computes masked coefficients but does not enable a clock, write registers, or verify PLL lock.

The host-only integration model (`PujjoBringupCoordinator`) requires independent evidence before simulating: mute DSP -> amp disabled -> DSP ready -> configure SSP1 -> confirm gain limiter -> enable amp -> allow first-time 300 ms calibration -> stream ready -> unmute. On stop, suspend, or error it attempts mute before shutdown, retaining fault state. **It is a mock-only contract, not a live Windows implementation; its readiness inputs are simulated and do not establish speaker safety.** The physical SDB polarity and first-activation calibration must still be verified on the target board.

## Implementation by subsystem

### 1. Board identification and ACPI resource guards

- `src/driver/PujjoBoardId.[ch]` and `PujjoBoardGuard.[ch]`: parse/check Google Pujjo/Pujjo1e SMBIOS information and reject other boards before sensitive DSP access.
- `src/driver/experimental/PujjoDeviceMap.[ch]`, `PujjoAcpiResources.[ch]`: portable, fail-closed validation of expected ACPI identity/path/resource evidence; not live kernel drivers.
- `pujjo_rt5682_probe.vcxproj` / `pujjo_rt1019_probe.vcxproj` and `src/driver/peripheral/`: KMDF PnP resource discovery and transport development projects, **not** finished audio drivers or installable packages.

A PnP node or a successful mock resource validator is not proof a real Windows `WdfIoTarget` is ready.

### 2. RT5682S codec (headset / microphone)

- Portable code models identity, register encoding, error handling, read guards and limited state transitions. Linux references use **16-bit register addresses and 16-bit values**; the documented identity register/value are `0x00FF` / `0x6749`.
- Windows SPB/I²C transport source supports Resource Hub connection handling and atomic address-write/read sequences (`IOCTL_SPB_EXECUTE_SEQUENCE` / `SPB_TRANSFER_LIST`), but its WDK ABI and physical functionality remain unverified.
- v0.0.16 improves wait-lock-protected powered/close lifecycle, short-transfer handling, probe lifetime, and a **calculation-only** PLL2 planner.
- Still missing: true reset/rail/bias/analog sequencing, verified clocks and slot format, masked programming, calibration/pop suppression, jack detect, headphone output, microphone capture, D0/D3 restore, driver-to-DSP binding and actual endpoint audio.
- A separate publicly available [RT5682S Windows source project](https://github.com/coolstar/rt5682s) was identified as a possible reference. **It has not been integrated**; it has its own Apache-2.0 and MS-PL licensing obligations and does not solve the missing SOF controller pipeline by itself.

### 3. RTL1019 / RT1019P amplifier (speakers)

- The Linux RT1015P-family driver models logical GPIO SDB enable/disable and about **300 ms first-power calibration**. Windows must not assume raw GPIO physical polarity from Linux's logical descriptor.
- `src/driver/experimental/PujjoRt1019pAmp.[ch]` models muted/off, ready, playing, sleep and latched faults with rollback; its normal build is **locked**. Mock callbacks require explicit host-only flags.
- `src/driver/peripheral/PujjoRt1019pGpioWindows.[ch]` is Windows Resource Hub GPIO transport source using `IOCTL_GPIO_WRITE_PINS`. It is **not wired to the playback stack** and does not authorize writes to raw ACPI pin 75.
- v0.0.16 improves synchronized open/close and powered-state checks; GPIO API talks in physical voltage levels rather than pretending unknown polarity is established.
- Still missing: real assigned GPIO connection, validated circuit polarity, mute and conservative gain limiting, first-enable recovery, coordinated D0/D3, speaker-safe verification and fault-off behavior on the device.

### 4. Intel SOF firmware, DMA and Windows audio endpoints

**Pujjo v0.0.12.0 checkpoint (2026-11-27):** the real DMIC capture bridge
now uses PortCls-owned dedicated CPU pages and independent private conversion
scratch; the hardware DMA ring remains separate. A new read-only ROM readiness
diagnostic collects PCI capability and controller-idle evidence, without ROM
execution, DMA publication or an audio endpoint. Firmware and recording remain
locked. See [v12 capture bridge and ROM readiness](docs/PUJJO_CAPTURE_BRIDGE_V12.md).

**Pujjo v0.0.11.0 source checkpoint (2026-11-27):** a durable one-shot
DMA journal, atomic descriptor ownership ledger, cold-controller ownership
audit and separate ROM-only compile candidate advance the firmware loader.
The platform DMA isolation/drain contract remains unqualified; the ROM
candidate refuses DriverEntry and the loader gate stays closed. No new
installable package or recording endpoint is provided.
See [v11 ROM boot review](docs/PUJJO_ROM_BOOT_V11.md).

**Pujjo v0.0.10.0 source checkpoint (2026-11-27):** the physical v9.2
allocation/preflight passed; the active firmware and DMIC capture source
now uses independent MDL backing with checked descriptor ownership and
release guards. Boot remains locked because IOVA/adapter lifecycle on
PnP loss is not qualified. See [v10 DMA lifetime review](docs/PUJJO_DMA_LIFETIME_V10.md).
No new installable or recording-capable package is provided.

- Candidate **Alder Lake-N** firmware filename: `sof-adl-n.ri` (not generic `sof-tgl.ri`). SOF publishes **different IPC3 and IPC4 variants** under different paths despite the shared filename; choosing the name does not prove runtime compatibility.
- v0.0.16 parser tested the metadata of one official Intel-signed ADL-N **IPC4 2.12** image: `$AE1` extended manifest, `$AM1` binary header and **25 `$AME` modules**. **Metadata parsing is not cryptographic authentication**, ROM acceptance or proof that Pujjo can boot it. Firmware bytes are intentionally not distributed with this project.
- Real WDF common-buffer DMA allocation now avoids substituting CPU physical addresses for device logical DMA addresses. Real DSP ROM boot, DMA descriptors, interrupts, IPC4 READY exchange, module discovery, graph/stream setup and teardown remain incomplete.
- Inherited Windows source contains IPC/PortCls/WaveRT scaffolding and generic HDA/SoundWire/DMIC/USB/HDMI components. **None of that establishes a working Pujjo speaker, headset or microphone endpoint**. The prototype Pujjo WaveRT speaker path is intentionally fail-closed.
- Neither firmware boot nor unverified real amplifier activation should be enabled merely to satisfy a compilation or test.

### 5. Power management and concurrency

The intended Windows implementation must serialize PnP ownership, D0/D3 entry/exit, I²C and GPIO requests, stream stop, surprise removal, timeouts and firmware state. v0.0.16 adds device-parented WDF wait locks and powered-state checks to both Resource Hub transports, with host-side simulated concurrency tests. **True Windows IRQL, WDF lock ordering, ISR/DPC and DMA quiescence remain unverified.** The mock coordinator is deliberately not linked into the production kernel driver.

## What changed in v0.0.17

After inspecting the actual GitHub Actions Debug and Release failure logs for runs #1 and #2, v0.0.17 fixes the proven `/kernel` and WDK Resource Hub mistakes, uses the documented Resource Hub path **byte-size**, and removes imported desktop CRT headers from the relevant KMDF codec/clock code. A kernel-build compile-time guard now rejects test-only RT5682S writes. Focused mock WDK type and Windows-targeted Clang syntax checks were added, alongside the original portable tests.

**Current locally verified results:** 26/26 offline commands passed; 80/80 GCC/Clang strict, sanitizer and GCC analyzer test variants passed; the separately supplied Linux topology and kernel-reference checks passed. See [`TEST_RESULTS_v0.0.17.md`](TEST_RESULTS_v0.0.17.md) for exact limitations and evidence. These test counts do not include any successful MSVC/WDK compilation. A fresh GitHub Actions run is still required.

## What changed in v0.0.16

Compared with v0.0.15, this release:

1. Hardened SOF IPC4 `$AM1` / `$AME` module metadata parsing, bounds and overflow checks, with malformed-image and deterministic fuzz tests.
2. Required WDF DMA common-buffer logical addresses instead of unsafe physical-address fallbacks.
3. Serialized Windows SPB and GPIO open/transfer/power/close lifecycles; improved resource rebalance and invalidation.
4. Added RT5682S PLL2 coefficient planning for measured input-clock candidates without doing I²C writes.
5. Fixed mock coordinator shutdown retries after a fault, without clearing the fault latch.
6. Updated the main DSP Visual Studio project to a **WDK/KMDF driver configuration** (documented KMDF 1.33); still unverified under MSVC.
7. Improved CI so portable checks and Windows Debug/Release build attempts run independently, retaining logs and marking any outputs **NOT INSTALLABLE / NOT HARDWARE VERIFIED**.

See [`CHANGELOG_PUJJO.md`](CHANGELOG_PUJJO.md), [`CHANGES.md`](CHANGES.md) and [`CODEX_RESULTS.md`](CODEX_RESULTS.md) for the full development record.

## Source layout

```text
.github/workflows/compile-windows.yml  # Manual hosted build workflow
sof_windows_driver.sln                 # Main Windows solution
sof_dsp.vcxproj / sof_core.vcxproj      # SOF projects
pujjo_rt5682_probe.vcxproj             # RT5682S resource/transport prototype
pujjo_rt1019_probe.vcxproj             # RTL1019 GPIO resource/transport prototype
src/driver/                            # Windows kernel / SOF implementation
src/driver/peripheral/                 # KMDF SPB / GPIO development code
src/driver/experimental/               # Fail-closed host-side Pujjo models
include/ , src/core/                   # Inherited SOF shared code
scripts/                               # Read-only diagnostics and development tools
tests/                                 # Portable C, Python and static tests
Build_Pujjo.pyw                        # Windows development GUI (not installer)
README.md                              # This consolidated guide
START_HERE.md                          # Short current-release entry point
CODEX_RESULTS.md, TEST_RESULTS.md       # Actual development/test evidence
REMAINING_BLOCKERS.md                  # Detailed outstanding work
RESEARCH_v0.0.16.md                    # Firmware and API provenance
LICENSE                                # Required copyright/license notices
```

The GitHub Actions workflow **must** be directly under the repository-root `.github/workflows/` directory, not inside a nested `Pujjo_SOF_Development_v0.0.16/` folder.

## Offline tests and current results

From the source root with **Python 3** and a suitable native C compiler, run:

```bash
python tests/run_offline_suite.py
```

On Linux, optional compiler/sanitizer variations include:

```bash
ASAN_OPTIONS=detect_leaks=0 python3 tests/run_compiler_matrix.py
```

Selected smaller checks can be run independently:

```bash
python tests/run_pujjo_tests.py
python tests/run_device_map_tests.py
python tests/run_acpi_resource_tests.py
python tests/run_topology_evidence_tests.py /path/to/chromebook-linux-audio-main.zip
python tests/run_rt5682s_codec_tests.py
python tests/run_rt1019p_amp_tests.py
python tests/run_pujjo_sof_fw_tests.py
python tests/run_bringup_tests.py
python tests/test_bringup_static.py
```

Real Linux topology/kernel archive checks need the **user's separate reference ZIPs**; these binaries are not part of this Windows-source package. The ACPI offline parser takes a separately supplied raw SSDT `.dat`:

```bash
python tools/analyze_acpi_audio.py --ssdt /path/to/SSDT.dat
```

### Recorded v0.0.16 results (2026-11-17)

| Check | Recorded result |
| --- | --- |
| Expanded offline allowlist | **24 commands passed**, 0 failures |
| GCC strict, sanitizer and analyzer matrix | **48 build/test variants passed** |
| Host-simulated WDF transport concurrency under ThreadSanitizer | Passed |
| Official ADL-N firmware **metadata** parse under ASan/UBSan | Passed; **no signature authentication** |
| Windows MSVC/WDK Debug and Release | **Not compiled**; MSBuild unavailable in the originating environment |
| Additional Clang matrix | Skipped in originating environment (compiler unavailable) |
| External Linux topology/kernel fixtures | Skipped where not supplied |
| Actual Chromebook audio, Driver Verifier and HLK | **Not run** |

These are recorded development results, **not proof of hardware readiness**. The reports note host WDF doubles, sanitizer limits and skipped cases; consult [`TEST_RESULTS.md`](TEST_RESULTS.md) and `validation-v0.0.16/` for the actual evidence. Historical logs and counts may describe older revisions.

## Compiling on Windows or GitHub Actions

### Local development build

**Prerequisites:** Windows x64, Visual Studio 2022 with C++ support, Windows SDK **10.0.26100.0**, matching Windows Driver Kit **10.0.26100.0**, and Python 3. Project files have not yet been verified with this toolchain.

Open `sof_windows_driver.sln` in Visual Studio, select **x64 / Debug** or **x64 / Release**, then attempt **Build Solution**. Alternatively launch the development GUI `Build_Pujjo.pyw` to run its tool checks or source-only compile action.

From a Visual Studio Developer PowerShell configured with the WDK:

```powershell
msbuild sof_windows_driver.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0
msbuild sof_windows_driver.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0
```

Expected outcome today: **unknown**. Windows-specific C/WDK build issues may still exist and must be fixed using real compiler errors. A successful build is a **compile-only milestone**, not permission to install or power hardware.

### GitHub Actions (recommended for first Windows build)

The existing [workflow](.github/workflows/compile-windows.yml) is manually dispatched and defines:

- `offline`: Ubuntu 24.04, runs the portable test suite and uploads `portable-results` logs.
- `build` matrix: Windows Server 2022, **Debug + Release x64**, provisions VS2022/MSBuild and attempts SDK/WDK 26100 installation.
- Separate logs/artifacts; future compiled products are explicitly labeled `NOT-INSTALLABLE-NOT-HARDWARE-VERIFIED`.

On GitHub, open **Actions → Pujjo source-only Windows compile → Run workflow** on the branch containing this repo-root workflow. Download logs for any red build job and address the first actual MSBuild/WDK error; the workflow has **not yet been confirmed successful**.

There is intentionally **no** driver-install, test-signing, firmware-upload, or speaker-power step in this workflow.

## Read-only hardware diagnostics

Optional Windows PowerShell commands from the source directory, on the actual Pujjo Chromebook, **do not install a driver or drive GPIO**:

```powershell
.\scripts\read_only_pujjo_check.ps1
.\scripts\read_only_acpi_audio_evidence.ps1
.\scripts\ReadOnly-PujjoSofPreflight.ps1
.\scripts\export_pujjo_audio_inventory.ps1 -OutputPath .\pujjo-inventory.json
```

These report SMBIOS/PnP identifiers and available firmware facts; they **cannot** prove live SSP routing, codec readiness, DSP startup, GPIO logical polarity or safe speaker gain. If Windows blocks script execution, inspect the script rather than weakening your security settings just to run it.

## Remaining work and safe development order

**Before any audible or installable hardware test**, the project needs at least:

1. **Windows compile/analysis:** reproducible WDK/MSVC Debug + Release builds; fix Windows ABI/IRQL/SAL/project/PortCls/KMDF errors; run CodeQL/appropriate driver analysis. Microsoft has retired Static Driver Verifier from WDK 26100.
2. **Board resources:** verify Pujjo/Pujjo1e hardware distinctions, actual PnP-assigned SPB/GPIO connections, SSP0/SSP1 pinmux, MCLK/BCLK/FSYNC, amplifier polarity, reset, rails and jack interrupt on real hardware.
3. **Trusted firmware boot:** finish signed ADL-N manifest/segment policy, ROM authentication/error handling, DSP reset/power and loader DMA, interrupt completion, bounded failure recovery and IPC4 READY/capability validation. **Metadata parsing alone does not authorize boot.**
4. **Windows DSP topology:** implement and validate IPC4 module/graph/SSP configuration and firmware-compatible pipeline setup, teardown, capture/render format, clock ownership and link safety; don't load the Linux `.tplg` directly.
5. **Real peripheral integration:** finish RT5682S codec initialization, headset/jack/mic and PLL lock; validate RTL1019 logical GPIO polarity, calibration, mute, safe gain and fault-off behavior. Never use fixed GPIO pin numbers for hardware access.
6. **Windows audio endpoints:** connect PortCls/WaveRT endpoints to functioning DSP streams with real render/capture DMA, position/timestamp handling, notifications, mute/volume, conservative limits and complete stop/removal cleanup.
7. **Concurrency and power:** prove D0/D3, ISR/DPC, DMA quiescence, cancellation, surprise removal and lock order under actual WDF; replace simulated coordination with a separately reviewed Windows cross-device design.
8. **Controlled validation only after the above:** use a recoverable development machine, hardware measurements, debugger, proper packaging/signing, Driver Verifier, suspend/resume/failure tests and appropriate audio certification. The daily-use Chromebook should not be the first experiment.

For tracked blockers and evidence see [`REMAINING_BLOCKERS.md`](REMAINING_BLOCKERS.md), [`CODEX_RESULTS.md`](CODEX_RESULTS.md) and [`RESEARCH_v0.0.16.md`](RESEARCH_v0.0.16.md).

## Licensing and upstream references

- Base Windows SOF project: [thesofproject/sof-windows-drv](https://github.com/thesofproject/sof-windows-drv). Preserve the bundled `LICENSE` and the individual source-file licenses; some shared code has specific dual-licensing or other notices.
- SOF firmware: [thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) and [SOF boot documentation](https://thesofproject.github.io/latest/developer_guides/firmware/boot.html). This ZIP does **not** redistribute the inspected firmware image.
- Linux hardware/codec references: [Linux kernel](https://github.com/torvalds/linux) (`rt5682s.c`, `rt1015p.c`, `sof_rt5682.c`, `pci-tgl.c`); **GPL and other SPDX obligations remain with those sources**. Do not paste GPL code into a differently licensed file without resolving compatibility and notices.
- Chromebook Linux audio reference: [WeirdTreeThing/chromebook-linux-audio](https://github.com/WeirdTreeThing/chromebook-linux-audio). Its `.tplg` provides reference labels, **not** a Windows-ready topology; the binary is not redistributed here.
- Optional public Windows codec reference: [coolstar/rt5682s](https://github.com/coolstar/rt5682s), subject to its own Apache-2.0/MS-PL attribution requirements and separate device validation. No such source has been imported here.
- Microsoft driver docs: [WDK build](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-a-driver), [WDF hardware resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/finding-and-mapping-hardware-resources), [SPB Resource Hub connection IDs](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/connection-ids-for-spb-connected-peripheral-devices), [SPB transfer lists](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/using-the-spb-transfer-list-structure), [GPIO resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-based-hardware-resources), [common buffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-common-buffers).

**In short:** v0.0.17 now compiles on Windows WDK in Debug and Release. It is still **not an installable or working audio driver**. See [PREINSTALL_READINESS.md](PREINSTALL_READINESS.md) for hardware-free validation and the missing real firmware/audio pipeline.


### Windows CI compile-fix iteration (after Actions run #1, 2026-11-17)

The first Windows Server 2022 GitHub Actions run completed Visual Studio/SDK/WDK
installation and passed its portable test job, but both MSVC builds failed.
Specific compiler diagnostics included `C4083`/`C4005` desktop runtime/kernel
header conflicts, `C2220` warnings-as-errors, and `C4013` for
`RESOURCE_HUB_CREATE_PATH_FROM_ID`.

This iteration:
- Introduces `src/driver/PujjoFixedTypes.h` for fixed-width types, using WDK
  `UCHAR`, `ULONG`, `SIZE_T` in kernel builds and standard C types in portable
  builds. This avoids importing desktop `stdint.h` from the two PnP/resource
  policy modules into KMDF builds.
- Removes `string.h` from the resource-validation code while preserving its
  failure-path zeroing behavior.
- Explicitly defines `_KERNEL_MODE` for the two KMDF peripheral probe projects.
- Emits a clear compile-time diagnostic if the WDK Resource Hub path interface
  is still missing instead of silently guessing a connection path.

**Not verified on MSVC/WDK yet.** Push this revision and rerun both Windows
jobs. Additional genuine source errors and link errors may become visible
once the first header problems are resolved. These changes do not activate
DSP firmware or write GPIO/I2C registers, and the source remains non-installable.
