# Codex task: audit, compile, and thoroughly test Pujjo SOF Windows audio driver v0.0.13

**Input:** `Pujjo_SOF_Development_v0.0.13_source_only.zip` (full project source).

**Target device:** Lenovo 100e Gen 4 Intel Chromebook / Google Pujjo or Pujjo1e board, Intel N100 / Alder Lake-N, Intel audio controller `PCI\VEN_8086&DEV_54C8`, Realtek `ACPI\RTL5682` (RT5682S codec), and `ACPI\RTL1019` (RT1019P-compatible speaker amplifier). This identification is based on previously collected device evidence; confirm against the checked-in documentation and do not generalize it to other hardware.

**Important:** v0.0.13 is experimental SOURCE ONLY, not a functioning or installable driver. Existing offline tests are NOT evidence that the Windows kernel driver, firmware, codec, or speakers work. Do not install anything on a physical device, enable test-signing, power up the amplifier, or bypass hardware safety gates as part of this task.

## Copy/paste this request into Codex

> Unzip the attached Pujjo SOF v0.0.13 project and read this `CODEX_INSTRUCTIONS_PUJJO_SOF_v0.0.13.md` file in full. Audit and improve the entire source tree, get all supported software tests passing, and compile the Windows driver projects with the appropriate Microsoft Windows SDK/WDK on a disposable Windows environment (prefer GitHub Actions `windows-2022`). Fix real issues rather than bypassing safety checks. Verify the Intel Alder Lake-N SOF firmware selection, the RT5682S and RT1019P integration scaffolding, and the build setup. Do NOT turn on hardware access or claim it works on my Chromebook. Keep a detailed changelog, include actual commands and logs, and return a new source ZIP plus a report listing what passed, failed, was skipped, and remains untested. Do not stop at a plan; implement and test as much as the available environment allows.

## Priority 1 — Understand the repository and protect the device

1. Extract the ZIP and inventory all projects, C/C++/Python files, build scripts, and included documentation. Start with `START_HERE.md`, `README_INTEL_SOF_FIRMWARE_DEVELOPMENT.md`, `README_RT5682S_DEVELOPMENT.md`, `README_RT1019P_DEVELOPMENT.md`, and `README_BUILD_FIRST.md`.
2. Distinguish code linked into actual WDK projects from stand-alone experimental/mock code. Document all unimplemented functions, unconnected hardware helpers, and assumptions in the inherited SOF stack.
3. Preserve **fail-closed defaults**: `PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT` and any separate IPC4-reviewed opt-in controls must remain disabled, as must physical codec register writes and GPIO amplifier activation. Never bypass these just to satisfy a build or test.
4. Do not silently alter signing, Windows security settings, boot options, device binding, driver installation, firmware flashing, ACPI, or Chromebook firmware.
5. Keep safe GPIO/Resource Hub handling: use PnP-translated resource connection IDs, not raw GPIO pin numbers; verify correct device identity and resource ownership before any possible future hardware work.
6. Identify licensing and redistribution boundaries. Do not copy GPL-only Linux implementation into BSD-licensed project code, or redistribute proprietary CoolStar binaries or unverified firmware blobs. Keep upstream license notices intact.

## Priority 2 — Clean up and correct the code

1. Run static analysis and inspect for unsafe memory management, null pointers, integer overflows, structure packing, buffer lengths, type mismatches, resource lifetime and cancellation errors, race conditions, concurrency/IRQL issues, incorrect WDF object parenting, power-state mistakes, and error-path cleanup.
2. Refactor brittle or duplicated logic while preserving behavior. Improve clear diagnostics and add small, focused regression tests for every bug fixed.
3. Verify the codec path: RT5682S 16-bit I²C/SPB framing, ID checks, register/clock planning, suspend/resume, failed transactions and safe defaults. Confirm which helpers are *not* yet connected to real KMDF PnP callbacks.
4. Verify the amplifier path: `RTL1019` is the RT1019P-compatible device mapped to the Linux `rt1015p` family, not necessarily the distinct `rt1019.c` device. Check GPIO resource mapping, polarity assumptions, the 300 ms initial delay, mute-before-enable, sequencing, rollback, D0/D3 transitions, and failure handling. Treat physical polarity and gain limits as **unverified**.
5. Verify the Intel N100 SOF firmware path: `sof-adl-n.ri` is the candidate Alder Lake-N file, while IPC3 and IPC4 images can share that name in different paths. Do **not** substitute `sof-tgl.ri` or assume any Linux topology `.tplg` is a valid Windows IPC4 graph. Inspect current manifest parsing and firmware/IPC compatibility; document any blockers rather than inventing compatibility.
6. Check Pujjo board guards and ensure the driver does not falsely identify another Intel chipset or present a silent successful playback endpoint.
7. Make the documentation and version references consistent (some older README sections still refer to previous versions). Mark every unfinished item clearly.

## Priority 3 — Run extensive automated tests

Execute, and fix genuine failures in, the available portable test suites (exact filenames confirmed in v0.0.13):

```bash
python tests/test_kmdf_security.py
python tests/test_pujjo_sof_fw_static.py
python tests/run_pujjo_sof_fw_tests.py
python tests/test_rt1019p_static.py
python tests/run_rt1019p_amp_tests.py
python tests/test_rt5682s_static.py
python tests/run_rt5682s_codec_tests.py
python tests/test_acpi_parser.py
python tests/run_acpi_resource_tests.py
python tests/run_device_map_tests.py
python tests/run_pujjo_tests.py
python tests/run_codec_init_tests.py
python tests/run_headset_clock_tests.py
python tests/verify_linux_topology.py
python scripts/verify_upstream_integrity.py
python tests/test_sof_zephyr_log.py
```

Additional expectations:
- Run C unit tests under available GCC/Clang and, where practicable, MSVC; use AddressSanitizer/UndefinedBehaviorSanitizer where compatible.
- Cover valid and malformed resource descriptors, wrong board IDs, absent firmware, wrong IPC selection, I²C bus failures, amplifier enable/mute failures, short reads/writes, repeated suspend/resume, initialization failures, and teardown/rollback. Use mocks only; no physical hardware I/O.
- Distinguish **PASS**, **FAIL**, **SKIP** and **NOT RUN**. The Zephyr logging suite previously skipped one external ELF-dependent test; do not count that as a pass without the fixture.
- Never change a test merely to mask a real defect. Capture the actual output, including failed commands.

## Priority 4 — Build on Windows (NOT on my Chromebook)

1. Inspect `.github/workflows/compile-windows.yml` and the Visual Studio `.sln` / `.vcxproj` targets. Fix invalid/fragile workflow steps and project references, without enabling installation.
2. Use a disposable hosted Windows worker with Visual Studio 2022 C++ and an appropriate matching Windows 11 SDK/WDK (the existing project targets 10.0.26100.0; verify tool availability and documented install procedure instead of assuming). The workflow currently has **not** been verified to run successfully.
3. Run a clean x64 Release build, capturing the complete MSBuild log:

```powershell
msbuild sof_windows_driver.sln /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0 /v:normal
```

4. Fix compiler, linker, SDK/WDK, annotation, and project configuration issues; repeat until build succeeds or a clear, reproducible external blocker is identified. Do not report a successful build unless the job actually completes successfully.
5. If possible, also build x64 Debug, run appropriate Windows static checks (e.g. CodeQL / MSVC analysis), and publish logs as CI artifacts. Label all `.sys` output **NOT INSTALLABLE / NOT HARDWARE VERIFIED**.
6. Do not create or run an installer or sign/deploy to the Chromebook. Build success is **not** evidence of successful DSP boot, speaker audio, or electrical safety.

## Priority 5 — Deliverables

Provide all of the following:

- A new, clearly versioned **complete source ZIP** that includes the original project plus reviewed changes, CI workflow, and tests. Do not call it a working driver.
- A `CODEX_RESULTS.md` report with environment details, exact commands, test counts and skip reasons, WDK/MSBuild build status, compiler/analyzer findings, and links to CI logs if available.
- A `CHANGES.md` or changelog entry showing each modified source file, what was fixed, and how it was validated.
- A `REMAINING_BLOCKERS.md` separating (a) what can be addressed in code, (b) what needs Windows-build verification, and (c) what **requires actual hardware**.
- A clear explanation of what is still necessary before even considering a controlled hardware test: verified Intel ADL-N firmware/IPC, valid Windows topology and SSP mapping, functioning Windows PnP I²C/GPIO integration, speaker-safe mute/volume, power handling, reversible installation/rollback, and separately confirmed test-environment safety.

## Completion criteria

Treat the cloud/software stage as complete **only** when the source is organized and reviewed, all feasible automated tests have documented results, Windows WDK build has actually been attempted, and remaining issues are explicitly listed. If the WDK build fails, report that truthfully and still provide the source with the failure log. Do not say the driver or speakers "work" without physical verification. Never require me to compile on my 64 GB Chromebook to finish the software-only stage.

## Practical setup

Put the **contents** of the source ZIP (not the ZIP itself) at the root of a GitHub repository that Codex can access, and place this Markdown file alongside `START_HERE.md`. Connect the repository to Codex and paste the request above. If Codex supports direct local-file input instead, provide both files and ask it to unpack the ZIP first. Keep your raw device dumps, identifying information, and BIOS/ACPI captures out of public repositories unless intentionally reviewed for privacy.
