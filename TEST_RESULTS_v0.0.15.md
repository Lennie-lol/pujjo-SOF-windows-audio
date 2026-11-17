# Pujjo SOF v0.0.15 test results — 2026-11-17

## Independently run, Linux host only
- Offline Python/C script commands: **21 PASS, 0 FAIL**, with 4 explicitly **SKIP/NOT RUN** external-fixture or physical-hardware commands.
- New bring-up coordinator: compiled and executed in **locked** and **host-only mock** modes with GCC and Clang, strict warning flags.
- Extra UBSan tests: GCC+Clang × locked+mocked = **4/4 PASS**.
- Negative compile test: attempting host-only callback opt-in with `_KERNEL_MODE` failed as intended.
- Mock fault injection: DSP mute, SDB-off, firmware readiness, SSP1 preparation, gain readiness, amplifier enable/calibration, DSP unmute, shutdown, and failed stream readiness.
- Tested transitions: RESET → PREPARED_MUTED → AMP_ON_MUTED → STREAMING, stop, suspend, resume requiring fresh readiness.

## Not tested / cannot claim
- Windows WDK/MSBuild x64 compile (toolchain unavailable), Windows ABI/IRQL/SAL, driver installation, Intel DSP firmware boot, real I2C/GPIO, actual speaker output, Windows Driver Verifier or HLK.
- No actual Chromebook was accessed and no kernel-mode binary was generated.

Earlier audit/test files are historical v0.0.14 output and must not be interpreted as v0.0.15 WDK test results.
