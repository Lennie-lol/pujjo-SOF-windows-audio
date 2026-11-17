# Pujjo SOF v0.0.14 — software audit results

Date: 14 November 2026. Input: v0.0.13 source-only ZIP in the supplied Codex package.
**SOURCE ONLY. NOT INSTALLABLE. No working Windows audio driver or speaker playback demonstrated.**

## Outcome

Fixed amplifier fault recovery, stale codec/resource validation data, short SPB transfers,
firmware parser length/offset handling, short firmware-file reads, false firmware-ready
success, and CI failure propagation. All boot, codec write and amplifier activation
opt-ins remain disabled. No physical device was accessed, installed, signed, or reconfigured.

This is a bounded audit of the Pujjo changes, loader, peripheral lifecycle, test tooling,
and build configuration, not an exhaustive proof of the entire inherited driver.
Inventory and exact project membership: `audit-logs/inventory.json`.

## Executed validation

| Check | Result | Scope |
|---|---|---|
| Explicit offline allowlist | PASS: 19 commands, 0 failures | Includes all 16 requested commands plus resource, loader/transport and audit regressions |
| Zephyr logging within those commands | 7 passed, 1 skipped | Real external Panther Lake ELF absent; the skipped case is NOT a pass |
| GCC strict build/run | PASS: 13 combinations | 12 C module/variant combinations plus extracted production loader/transport functions |
| GCC ASan + UBSan build/run | PASS: 13 combinations | `detect_leaks=0`; no reported address/undefined-behavior errors |
| GCC `-fanalyzer` build/run | PASS: 13 combinations | No diagnostics at the configured warning level; limited to portable modules and host doubles |
| Regression checks against original source | Expected FAIL: 4 suites | Amp, codec, resource-policy, loader/transport regressions expose original defects |
| Upstream integrity | PASS: 48 tracked files unchanged | SHA-256 verification, also included in offline allowlist |
| Python syntax / project references / YAML | PASS | Syntax and source existence only; no WDK API validation |
| Actual Windows WDK compile | BLOCKED / NOT RUN | Local command attempted, shell exit 127: `msbuild: command not found` |
| Clang / MSVC | SKIP / NOT RUN | Not installed in this execution environment |
| LeakSanitizer | BLOCKED | Runtime reports unsupported ptrace environment; initial failures retained |
| Real topology ZIP / Linux source ZIP checks | SKIP | User-provided external archives absent |
| Driver Verifier, HLK, CodeQL, Windows `/analyze`, hardware | NOT RUN | No Windows worker or physical DUT used |

The 39 C combinations repeat the same modules under different compiler options; they
are not 39 independent functional features. C suites use assertions rather than a test
framework, so no invented per-assertion pass total is reported. The loader harness also
executes 20,000 deterministic malformed-manifest cases per profile; codec tests exercise
1,000 suspend/resume/identify cycles. These are synthetic, not real firmware validation.

## Reproduction and logs

Environment: Debian GNU/Linux 13, x86-64, Python 3.12, GCC 14. Exact versions and kernel:
`audit-logs/environment.log`. Run from the extracted source root:

```sh
python tests/run_offline_suite.py
ASAN_OPTIONS=detect_leaks=0 python tests/run_compiler_matrix.py
```

`audit-logs/final/summary.json` lists each command, exit code and skipped-case count.
Each adjacent `.log` contains the command and actual output. `audit-logs/matrix/`
contains all 39 compile/run logs, flags and statuses. `matrix-initial-lsan-failure/`
preserves the first sanitizer failures; these were infrastructure failures, not suppressed
code defects. Address and undefined-behavior instrumentation stayed enabled on the rerun.

`audit-logs/baseline*.log` preserves initial checks. The initial broad discovery attempted
the inherited DUT Python script, but it stopped at `ModuleNotFoundError: aphid` before
any hardware access. Final execution uses an explicit offline allowlist. Both inherited
Python DUT runners are now disabled at entry and safe to import. PowerShell DUT scripts
remain historical source and were not executed. The external topology runner initially
failed because its required ZIP was missing; this is recorded as SKIP in final results.
`loader-harness-initial-failure.log` preserves a host-double unused-variable build issue,
corrected by making the fake descriptor consume its buffer argument.

The Windows command actually attempted was:

```text
msbuild sof_windows_driver.sln /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:TargetPlatformVersion=10.0.26100.0 /v:normal
```

See `audit-logs/windows-build-attempt.log`. There is no MSBuild/WDK here and `gh auth
status` reports no authenticated host. Available connected GitHub tools expose no direct
workflow-dispatch operation; no repository was changed to trigger CI. No hosted Windows
build took place, and there is no CI run URL. The requested cloud/software completion
criterion requiring an actual Windows build is therefore **not met**.

The revised manual GitHub Actions workflow provisions VS2022 WDK integration, installs
Microsoft's matching SDK/WDK, checks toolset/header availability, runs the offline
allowlist, builds Release and Debug, and uploads logs on failure. Microsoft lists
26100.6584 for VS2022; project header/library paths remain 10.0.26100.0. The workflow
has only been syntax/source checked, not executed. Its installer, toolchain and kernel
compile stages remain unverified. It contains no device deployment step.

## Code linkage and findings

- `sof_dsp.vcxproj`: actual inherited Windows PCI/DSP project, including board guard,
  Device, Loader, IPC, topology, WaveRT, PortCls, HDA, SoundWire and USB helpers. D0 entry
  and DSP boot refuse startup by default. Speaker SSP stream creation remains blocked.
- `sof_core.vcxproj`: builds only `src/core/sof_core_init.c`. The many inherited Linux
  reference source files are **not** automatically linked into the Windows driver.
- `pujjo_rt5682_probe.vcxproj`: links resource policy, board guard/ID, peripheral PnP,
  codec core and SPB transport. The read-only PnP opt-in is disabled; transports stay
  unopened. Codec writes remain test-macro-only.
- `pujjo_rt1019_probe.vcxproj`: links resource discovery and GPIO helper, not the portable
  amplifier state machine. No PnP callback calls the GPIO open/write helper.
- Other experimental policy, topology, audio-init and clock modules are standalone host
  models; they do not implement Windows playback. `src/tests/main.c` is not a solution
  target; its corrected firmware fixture was not executed as a complete Windows suite.
- Resource Hub connection IDs are preserved. No raw GPIO pin/address substitution was
  added. SDB polarity, GPIO grouping/ownership and gain remain unknown on hardware.
- RT5682S framing remains big-endian 16-bit register + 16-bit value; the read callback
  now requires all four SPB sequence bytes. Short writes already failed. Open/close,
  error cleanup and timeout paths were inspected, but kernel scheduling, cancellation,
  concurrency and live D0/D3 behavior were not validated by host doubles.
- Amp mute-before-enable and 300 ms first-enable model remain unchanged. A FAULT is now
  latched across stop/suspend retries until explicit reset/reinitialization; emergency
  shutdown still attempts both mute and SDB disable.
- ADL-N candidate remains `sof-adl-n.ri` with separate IPC3/IPC4 paths. No firmware was
  fetched or redistributed. Parser now uses the checked-in cAVS2.5 IPC4 binary-header
  offset `0x2000` within payload and validates its length, rather than treating CSS bytes
  as version metadata. It is not a signature, module-table, or real-image compatibility
  validator. Other layout families require explicit handling.
- The disabled boot branch no longer converts a FW_READY timeout into successful boot.
  That kernel path and short file-read cleanup are source-checked only, not executed on
  Windows. DMA fallback/IOMMU correctness and full failure rollback remain audit blockers.

## References used

- [Microsoft SPB sequence completion and short/NACK behavior](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/spbcx/nc-spbcx-evt_spb_controller_sequence)
- [Microsoft supported WDK/SDK downloads](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads)
- [Microsoft WDK VS integration requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- [VS2022 WDK component identifiers](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-enterprise?view=vs-2022)
- Checked-in dual-licensed `include/sof/ext_manifest4.h`, `src/core/intel/tgl.c` and
  `src/core/ipc4-loader.c` establish the header offset reference. Their tracked bytes
  were not changed; no GPL-only implementation was imported.

Existing BSD and upstream dual-license notices are retained. No proprietary CoolStar
binary, new vendor code, firmware blob, INF, CAT or SYS is included.
