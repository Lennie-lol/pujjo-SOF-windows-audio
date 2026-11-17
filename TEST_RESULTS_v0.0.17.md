# Pujjo SOF v0.0.17 — local validation results (2026-11-17)

## Release gate

All checks **available in this Linux build environment** must pass before packaging. These checks cannot establish successful Microsoft WDK compilation, driver signing, Windows installation, or real Pujjo audio operation.

| Check | Result | Evidence or command |
| --- | --- | --- |
| Portable offline regression suite | **26/26 PASS** | `python3 tests/run_offline_suite.py --logs validation-v0.0.17/offline` |
| GCC / Clang compiler matrix | **80/80 PASS** | `python3 tests/run_compiler_matrix.py` (strict, ASan+UBSan, GCC analyzer, execution) |
| Fake-WDK kernel type and codec/clock testing | **PASS**, included in offline 26 | `python3 tests/run_kernel_mock_abi_tests.py` (8 compile-and-run, 4 Windows-targeting Clang syntax, 2 expected compile failures) |
| Source-level CI regression contract | **PASS**, included in offline 26 | `python3 tests/test_windows_ci_v017.py` |
| User-supplied Linux topology reference | **PASS** | `python3 tests/run_topology_evidence_tests.py /mnt/data/chromebook-linux-audio-main.zip` |
| User-supplied Linux kernel source reference | **PASS: 9 evidence checks** | `python3 tests/verify_kernel_reference.py /mnt/data/linux-master.zip` |
| Python source syntax validation | **PASS** | Python `ast.parse` across project scripts and tests |
| ZIP integrity and SHA256 manifest validation | **PASS** | Independent local packaging verification |
| **MSVC/WDK Debug+Release builds of this updated revision** | **NOT TESTED** | GitHub Actions Windows runner must compile the new commit |
| Physical Pujjo Chromebook / real DSP / audio | **NOT TESTED** | No verified installable driver or hardware path |

### Scope of test results

- The 26 offline jobs include host-mode simulated transports and firmware parser tests, not electrical or real WDF behavior. The host mock WDK headers and Clang Windows-target syntax are not the Microsoft compiler or WDK headers.
- The compiler matrix exercises 80 compiled host cases; it is **not 80 separate Windows drivers**.
- The two external Linux reference archives were available in the development session and tested separately. GitHub Actions does not automatically have those ZIP archives; the portable CI suite reports them as optional skips rather than downloading or redistributing the sources.
- Hardware-dependent power/regression scripts remain disabled and were not run, by design.
- The previous real Windows Actions runs, before these fixes, were both unsuccessful: see GitHub Actions runs `44091076316` and `44126964895` in `Lennie-lol/pujjo-SOF-windows-audio`.

### Next milestone

Push this source-only revision to the root of the GitHub repository and run `.github/workflows/compile-windows.yml` again. Fix any additional real MSVC/WDK compiler or linker failures; only then can the WDK build be marked successful. Hardware activation is a separate, much later milestone.
