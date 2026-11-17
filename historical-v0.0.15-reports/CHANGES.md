# Changes — v0.0.14

Original source retained; no firmware/binaries or device installation added.

## Modified files

- `.github/workflows/compile-windows.yml` — Fail on test errors; provision matching SDK/WDK and VS integration; Release/Debug matrix; always upload logs; no deployment.
- `Build_Pujjo.pyw` — Use the complete explicit offline runner instead of a partial test list.
- `PACKAGE_VERIFICATION.md` — Current-release banner and historical-status clarification.
- `README.md` — Current-release banner and historical-status clarification.
- `README_BUILD_FIRST.md` — Current-release banner and historical-status clarification.
- `README_INTEL_SOF_FIRMWARE_DEVELOPMENT.md` — Current-release banner and historical-status clarification.
- `README_RT1019P_DEVELOPMENT.md` — Current-release banner and historical-status clarification.
- `README_RT5682S_DEVELOPMENT.md` — Current-release banner and historical-status clarification.
- `START_HERE.md` — Current-release banner and historical-status clarification.
- `TEST_RESULTS.md` — Current-release banner and historical-status clarification.
- `src/driver/Loader.c` — Bound 32-bit image sizes, manifest and binary-header lengths; correct cAVS2.5 header offset; reject short file reads; fail FW_READY timeout.
- `src/driver/experimental/PujjoRt1019pAmp.c` — Keep amplifier FAULT latched after successful emergency stop; suspend cannot clear it.
- `src/driver/experimental/PujjoRt5682sCodec.c` — Clear verified identity on test write/verification failures.
- `src/driver/peripheral/PujjoResourcePolicy.c` — Clear stale resource summary on invalid device/input.
- `src/driver/peripheral/PujjoRt5682sSpbWindows.c` — Require full sequence transfer count; reject partial success/NACK.
- `src/tests/main.c` — Correct unexecuted Windows firmware fixture to payload + 0x2000 with binary header length/module count.
- `tests/run_headset_clock_tests.py` — Respect CC compiler selection.
- `tests/run_regression_suite.py` — Disable inherited remote hardware utility before any action; safe import.
- `tests/test_pujjo_resource_policy.c` — Verify failed validation clears a previously successful resource summary.
- `tests/test_rt1019p_amp.c` — Regression for fault stop/suspend/re-enable bypass.
- `tests/test_rt5682s_codec.c` — Verify failure clears identity and repeat 1,000 suspend/resume cycles.
- `tests/test_s0idle_and_d3.py` — Disable inherited remote hardware utility before any action; safe import.

- `src/driver/Loader.h` — Document scoped cAVS2.5 metadata parsing and its limitations.
- `CHANGELOG_PUJJO.md` — Add v0.0.14 entry linking current results.

## Added files and validation

- `tests/run_offline_suite.py`: 19-command explicit allowlist, durable logs, nonzero failure exit and skip accounting.
- `tests/run_resource_policy_tests.py`: connects existing resource C tests to automation.
- `tests/run_loader_transport_tests.py`: extracts exact production function bodies into a host API-double harness; exercises malformed manifest sizes/offsets, short/error SPB reads/writes and 20,000 bounded deterministic mutations.
- `tests/run_compiler_matrix.py`: GCC/Clang discovery, strict/sanitizer/analyzer builds; 39 GCC combinations passed. Clang absent.
- `tests/test_audit_regressions.py`: source-only checks for kernel short-read cleanup, boot timeout, project references, disabled opt-ins and CI logging.
- `CODEX_RESULTS.md`, `REMAINING_BLOCKERS.md`, `VERSION`, `CHANGES.md`: current status and exact limitations.
- `audit-logs/`: baseline, failures, final results, regression-on-original proof, inventory and review diff.
- Original `CODEX_INSTRUCTIONS_PUJJO_SOF_v0.0.13.md` retained for task provenance.

Regression validation: four added/strengthened suites fail against the original source;
the final offline run passes. Kernel file I/O and FW_READY changes have static coverage
only, not runtime Windows validation. Sanitizers cover portable code and API doubles,
not the WDK driver. Complete results and skip reasons are in CODEX_RESULTS.md.
