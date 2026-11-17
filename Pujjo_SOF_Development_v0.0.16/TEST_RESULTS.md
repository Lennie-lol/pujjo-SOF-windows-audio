# Automated results — v0.0.16

Date: 2026-11-17. Host: Linux x64, GCC 14.2.0, Python 3.12.14. No DUT attached or used.

| Check | Result | Evidence |
| --- | --- | --- |
| Original v0.0.15 allowlist | 21 commands passed | validation-v0.0.16/baseline |
| Final expanded allowlist | 24 commands passed, zero failures | validation-v0.0.16/final/summary.json |
| GCC strict, ASan/UBSan, -fanalyzer | 48 build/test variants passed | validation-v0.0.16/matrix/summary.json |
| ThreadSanitizer full transport test | Passed, pthread/WDF simulation only | validation-v0.0.16/thread-sanitizer.log |
| Real official ADL-N firmware parser + ASan/UBSan | Passed metadata parsing; no signature verification | validation-v0.0.16/real-firmware.log |
| LeakSanitizer extra attempt | Blocked by host ptrace restrictions | validation-v0.0.16/leak-sanitizer.log |
| Windows Debug x64 | Blocked: msbuild executable absent, no compiler ran | validation-v0.0.16/windows-Debug.log |
| Windows Release x64 | Blocked: msbuild executable absent, no compiler ran | validation-v0.0.16/windows-Release.log |
| Clang matrix | Skipped: compiler unavailable | matrix/summary.json |
| External topology/kernel archive checks | Skipped: reference archives absent | final/summary.json |
| Zephyr log ELF-dependent case | One skipped subcase; remaining cases passed | final/test_sof_zephyr_log.log |
| Physical DUT, Driver Verifier, HLK, MSVC analysis | Not run | No Windows/DUT environment |

Counts are commands/build variants, not independent hardware tests; matrix executions repeat portable cases with different compiler settings. ASan/UBSan matrix uses `ASAN_OPTIONS=detect_leaks=0`. Full transport tests compile the actual transport C files with replacement WDF headers/APIs. Loader/allocator harnesses extract actual function bodies, but still use host ABI mocks. No kernel ABI, real IRQL, DMA, electrical, sound or firmware-authentication claim follows.

Development failures are retained rather than hidden: initial transport harness lacked an include path; initial static check matched a comment; a temporary coordinator test incorrectly supplied 2 to a one-bit field and was corrected to test a missing flag. Affected suites were rerun successfully. Historical audit-logs and v0.0.15 reports are not current-release results.

Reproduce the extra firmware check after obtaining the exact image described in RESEARCH_v0.0.16.md:

```
PUJJO_FIRMWARE_TEST_FILE=/path/to/sof-adl-n.ri CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie' ASAN_OPTIONS=detect_leaks=0 python3 tests/run_loader_transport_tests.py
CFLAGS='-O1 -g -fsanitize=thread -no-pie' python3 tests/run_windows_transport_tests.py
```
