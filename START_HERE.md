# Pujjo SOF v0.0.16 — source-only development

Read CODEX_RESULTS.md, CHANGES.md, TEST_RESULTS.md and REMAINING_BLOCKERS.md first. This is not an installable or working Windows audio driver. No Windows binary was built, and no hardware was accessed.

Run `python tests/run_offline_suite.py` from this directory. On Linux with GCC, run `ASAN_OPTIONS=detect_leaks=0 python3 tests/run_compiler_matrix.py`. These are portable simulations, not Windows driver builds.

The manual `.github/workflows/compile-windows.yml` targets VS2022, SDK/WDK 10.0.26100 and Debug/Release x64 on a disposable hosted Windows runner. It has not run for this release. Keep all live DSP/amp/write gates disabled. Do not follow installation examples in preserved upstream documentation.

The v0.0.15 coordinator remains a host-only model. v0.0.16 improves real transport source, parsing, DMA allocation policy, clock planning, tests and build configuration; it does not complete the audio pipeline.
