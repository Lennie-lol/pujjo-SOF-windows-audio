# Windows build debugging — v0.0.17 (no version bump)

## Outcome

Prepared a focused fix for the latest actual failure. **The revised Windows build is not verified.** The GitHub integration refused branch creation with HTTP 403, the browser was signed out, and this Linux host has no MSBuild/WDK. No remote change, new run, merge, installation or hardware activation was performed.

## Baseline and latest CI evidence

Baseline: `6013d5da201bc10fc0167f885a180320492853bf` on `Lennie-lol/pujjo-SOF-windows-audio/main`, matching the attached Resource Hub hotfix source after normalizing line endings. The solution contains `sof_core` and three driver projects. The full tree, solution/project configurations, consolidated README, existing tests and current CI outputs were inspected for this focused build fix.

[Latest failed run #4](https://github.com/Lennie-lol/pujjo-SOF-windows-audio/actions/runs/44145039516), 17 November 2026:

| Configuration | Job | Actual result |
| --- | --- | --- |
| Release x64 | 134257749369 | All four outputs linked; 0 warnings; 3 TestSign errors |
| Debug x64 | 134257749374 | All four outputs linked; 0 warnings; 3 TestSign errors |
| Offline | 134257748812 | Passed |

The first actual errors are `SIGNTASK : SignTool error : No file digest algorithm specified`, one per `.sys` project. The Resource Hub helper hotfix **did resolve the prior compiler errors for this run**. There are no C4013/C1189 failures or unresolved linker symbols in these latest logs. This is evidence that the baseline compiled and linked, not evidence of an overall successful build or functioning audio.

Full original job logs and the original Debug/Release artifact ZIPs (MSBuild text/binlogs, kit installation logs, toolset list) are in `validation-build-debug/`. Their SHA256 digests were checked against GitHub's artifact metadata. No driver binaries were uploaded by that failed run, so none can be supplied from it.

## Focused changes

| File | Change and reason |
| --- | --- |
| `sof_dsp.vcxproj` | Set `SignMode=Off` before importing build targets; unsigned compile-only output instead of unintended default test signing |
| `pujjo_rt5682_probe.vcxproj` | Same explicit signing mode |
| `pujjo_rt1019_probe.vcxproj` | Same explicit signing mode |
| `.github/workflows/compile-windows.yml` | Add push trigger limited to `codex/wdk-build-fix`, retain manual dispatch; require all three `.sys` outputs and `sof_core.lib`, check that drivers are unsigned, record hashes and add NOT-INSTALLABLE label |
| `tests/test_windows_ci_v017.py` | Require explicit unsigned build policy; require Resource Hub helper feature definition before including its header |
| `README.md` | Correct current build status and link this report |

No C/C++ runtime implementation, warning policy, safety gate, license or project version was changed. `sof_core.vcxproj` and the solution require no change for the observed failure. Off is Microsoft's documented signing mode for building without a certificate. This does **not** disable Windows signature enforcement, create an installable package or authorize loading. Adding `/fd SHA256` would instead continue test-signing, contrary to the intended unsigned compile-only deliverable.

References: [Microsoft driver signing properties](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/driver-signing-properties), [building with WDK](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-a-driver). Actual SDK/WDK 26100 compilation in run #4 provides direct evidence for the Resource Hub declarations; no mock-header result is presented as WDK evidence.

## Tests executed on revised source

- 26/26 offline suite commands passed after installing an isolated Debian Clang 19.1.7 toolchain. The first attempt failed solely because Clang was absent; that failure log is retained.
- 80/80 GCC 14.2 and Clang 19.1.7 variants passed: strict warnings, AddressSanitizer/UBSan and GCC `-fanalyzer`.
- Kernel-type mock suite passed eight GCC/Clang build-and-run cases, four Windows-target Clang syntax checks and two negative tests rejecting kernel test-only codec writes. These are included within the 26-command suite, not additional WDK builds.
- ThreadSanitizer passed the pthread/WDF-doubles transport test.
- Python syntax, project XML, workflow YAML, patch whitespace and upstream integrity checks passed. The new signing assertion detects the absent setting in the original project.
- ASan matrix uses `detect_leaks=0` because this environment runs under ptrace; no LeakSanitizer success is claimed.
- One ELF-dependent Zephyr subcase and external topology/kernel-archive checks are skipped where their fixtures are absent. DUT, Windows Driver Verifier, CodeQL/HLK and live IRQL/power/audio tests were not run.

The 80 variants repeat algorithms under different flags. All mock tests remain simulations. Machine-readable results and complete command output accompany this report. Historical v0.0.16/v0.0.17 reports are not fresh results of this debugging task.

## Exact blocker and how to continue

Attempted `create_branch(codex/wdk-build-fix, 6013d5da...)` using the connected Lennie-lol account. GitHub returned:

```
HTTP 403
Resource not accessible by integration
POST /repos/Lennie-lol/pujjo-SOF-windows-audio/git/refs
```

Repository metadata reports the user's push permission, but the integration cannot perform that write. The local `gh auth status` is unauthenticated. The GitHub browser showed Sign in and no authenticated session. Local Debug and Release msbuild attempts both failed before compilation because the executable is absent. Exact evidence is retained in `validation-build-debug/access-and-environment.json` and local Windows attempt logs. Retrying unchanged CI would not test this patch, so it was not substituted for a new build.

To continue automatically, provide a GitHub connection authorized for repository contents/workflow changes and Actions execution, or apply this prepared patch yourself. Never provide a token/password in chat.

From a clean checkout at the baseline (or first check applicability against newer code):

```sh
git switch -c codex/wdk-build-fix
git apply --check /path/to/Pujjo_SOF_v0.0.17_WDK_build_fix.patch
git apply /path/to/Pujjo_SOF_v0.0.17_WDK_build_fix.patch
git add sof_dsp.vcxproj pujjo_rt5682_probe.vcxproj pujjo_rt1019_probe.vcxproj .github/workflows/compile-windows.yml tests/test_windows_ci_v017.py README.md BUILD_DEBUG_REPORT.md
git commit -m "Build unsigned Pujjo development drivers and verify all outputs"
git push -u origin codex/wdk-build-fix
```

The branch push triggers the workflow. Alternatively use Actions > Pujjo source-only Windows compile > Run workflow and select the changed branch. Require **both** Windows jobs and the offline job green, all four output files present and no unresolved errors. Inspect the new logs and address any newly revealed errors before claiming completion. This patch was not run on Windows; the added product-verification step is likewise unverified on the runner.

## Audio functionality remains incomplete

Signed SOF firmware authentication/ROM boot and IPC4 readiness, real DSP graphs/SSP routing, full RT5682S initialization/PLL/calibration/jack/microphone integration, RTL1019 GPIO polarity/calibration/mute/gain validation, PortCls/WaveRT render/capture, and real power/removal/DMA synchronization remain incomplete or hardware-unverified. Build success would only be a compile milestone. Keep DSP/codec/amp activation disabled. Any future development binaries must be labeled **NOT INSTALLABLE — NOT HARDWARE VERIFIED**.
