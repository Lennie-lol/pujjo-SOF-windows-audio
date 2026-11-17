> **Current release: v0.0.14 — SOURCE ONLY, NOT INSTALLABLE.** See [CODEX_RESULTS.md](CODEX_RESULTS.md), [CHANGES.md](CHANGES.md) and [REMAINING_BLOCKERS.md](REMAINING_BLOCKERS.md). Run `python tests/run_offline_suite.py` for the full offline allowlist. Windows WDK compilation remains unverified.

> The material below is retained release history; earlier version numbers and test counts describe those earlier packages, not v0.0.14.

# Pujjo RT1019P / RTL1019 speaker amplifier – v0.0.12

**DEVELOPMENT SOURCE ONLY. No installable or hardware-tested audio driver.**

## What we know and what we do not

- The Google Pujjo ACPI data reports `RTL1019` at `\_SB.PCI0.I2C3.ALCP`, with a **GPIO I/O** resource (ACPI pin 75 and `sdb-gpios`), not an I2C device transaction resource. This is static firmware evidence, not a verified Windows translated Resource Hub connection. Do **not** directly access pin 75.
- Linux `sound/soc/codecs/rt1015p.c` handles `RTL1019` / RT1019P-compatible GPIO-SDB power, with 300 ms first calibration delay. Linux uses a logical GPIO descriptor (which can invert the electrical level), so Windows must **not** assume raw physical voltage or active-high polarity.
- Candidate Linux speaker path: SSP1; it does not prove that the current Windows Intel SOF firmware or IPC4 topology supports playback.
- **Unknown / unverified:** live Windows GPIO connection ID and pin grouping, electrical SDB polarity, Intel firmware/IPC topology, speaker gain and mute safety, and real-world behavior over sleep/resume.

## Files changed in this release

| File | Purpose | Status |
| --- | --- | --- |
| `src/driver/experimental/PujjoRt1019pAmp.h/.c` | Checked state transitions: muted-off, powered-muted, playing, suspend, fault; 300 ms delay; rollback on errors | Host C tests only |
| `src/driver/peripheral/PujjoRt1019pGpioWindows.h/.c` | Resource Hub `WdfIoTarget` GPIO source using `IOCTL_GPIO_WRITE_PINS`, never hardcoded physical pins | Windows WDK compile NOT verified; unused by live PnP driver |
| `pujjo_rt1019_probe.vcxproj` | Includes the GPIO transport source in the WDK project | WDK compile NOT verified |
| `tests/test_rt1019p_amp.c` | Locked default mode; mock power, audio gating, mute-before-disable, failure rollback, re-calibration on resume | Host-tested |
| `tests/run_rt1019p_amp_tests.py` | Runs portable C tests with Clang/GCC host compiler | Host-tested |
| `tests/test_rt1019p_static.py` | Verifies that no call from current PnP driver can power on the amp; no INF/SYS in source archive | Static checks only |

## Safety design

1. The normal compile of the amplifier state machine returns `PJ1019_LOCKED` before calling any GPIO or speaker callbacks. Only `PJ1019_TEST_ONLY_CALLBACKS` **and** `PJ1019_TEST_BUILD` together permit mock callbacks in isolated tests. These macros are NOT enabled by the Windows project.
2. Mock sequence is DSP mute first, SDB logical enable, one-time 300 ms delay, **remain muted**, then unmute only with an explicit streaming-ready signal. On shutdown, mute DSP before SDB logical disable; even when muting fails, disable is still attempted. Failure enters `PJ1019_FAULT` and cannot start again without explicit reinitialization.
3. Suspend requires shutdown and marks the amplifier as needing recalibration after resume. Resume never automatically enables SDB. This is a model only, **not** a verified D0/S0ix implementation.
4. The standalone Windows GPIO transport source is compiled separately but **is not called** from `PujjoPeripheralKmdf.c`. It has no INF, installer, driver entry for audio control, or link to the playback stack. Merely having its `Write` method does not make it safe to call on real hardware.
5. `pj1019_evidence_complete()` models conditions needed for a future test. The all-true flags used by unit tests are **fake**. No code here actually verifies those flags on the Chromebook. There is NO working amp activation path in this package.

## Run tests without the WDK

```
python tests/run_rt1019p_amp_tests.py
python tests/test_rt1019p_static.py
python tests/test_kmdf_security.py
```

The ZIP includes the full inherited source from v0.0.11 and the older codec tests.

## What remains before physical amplifier testing

- Obtain a successful Windows WDK 26100 MSBuild and static analysis of this new KMDF source; correct any build errors. Hosted CI workflow is included, but no successful run is claimed.
- Confirm PnP-assigned RTL1019 Resource Hub GPIO connection and expected **one-pin** grouping, logical SDB polarity and hardware owner, without hardcoded GPIO numbers.
- Finish and validate board-specific SOF firmware/IPC version, SSP speaker format, safe DSP muting and gain limits, and power coordination. Audio readiness must be proven before any unmute.
- Implement audited PnP D0 entry/exit wiring, exclusive GPIO target ownership, recovery and rollback policy, and playback coordination (currently unconnected).
- Run Windows Driver Verifier, system recovery tests and hardware tests on a separate recoverable test device before trying anything on the Chromebook used for school.

## Documentation consulted

- Microsoft: https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-based-hardware-resources
- Microsoft: https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/connecting-a-kmdf-driver-to-gpio-i-o-pins
- Microsoft: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpio/ni-gpio-ioctl_gpio_write_pins
- Linux upstream reference `sound/soc/codecs/rt1015p.c` (GPL-2.0-only); **not copied** into this BSD-3-Clause module.

No change in this release claims successful actual speaker amplification or Windows loading.
