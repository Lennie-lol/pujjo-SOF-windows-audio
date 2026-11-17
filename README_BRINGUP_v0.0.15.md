# Pujjo Windows audio v0.0.15: software-only integration contract

**SOURCE ONLY — NOT AN INSTALLABLE AUDIO DRIVER.** This development revision does **not** load firmware, toggle real GPIO, program a codec or play audio. It does **not** include a signed SYS/CAT/INF for installation. The kernel DSP boot gate stays locked.

## Why this code exists

The original source contains independently testable RT5682S codec identification, RT1019P/RTL1019 amplifier lifecycle, and Intel ADL-N SOF firmware policy. Those separate parts do not establish a working Windows audio path. `PujjoBringupCoordinator.[ch]` adds a **portable, deterministic, mock-only SSP1 speaker coordination model** to make the prerequisites and ordering independently testable.

### Added source

- `src/driver/experimental/PujjoBringupCoordinator.[ch]` — validates board identity and separate **speaker**, **amplifier**, and **firmware** evidence before modeled startup; models mute -> amplifier off -> DSP ready -> SSP1 config -> gain-limiter readiness -> amp enable -> first-time 300 ms calibration -> stream ready -> DSP unmute. Stops and suspend transitions mute before disabling SDB. A callback failure latches a fault and attempts both mute and shutdown.
- `tests/test_pujjo_bringup.c` — verifies wrong board, missing/invalid evidence, full sequence, calibration once per power epoch, suspension and fresh evidence, negative DSP-readiness, and failures during startup/stream/shutdown.
- `tests/run_bringup_tests.py` — compiles and runs a locked build and a separate host-only mock build. Confirms the kernel guard fails compilation if somebody tries to enable mock callbacks in kernel mode.
- `tests/test_bringup_static.py` — prevents host-only callback flags or the mock module being silently wired into a WDK project; checks the existing DSP boot lock remains present.
- `tests/run_offline_suite.py` — now includes both new suites. Existing compiler, codec, amp, ACPI and firmware tests are preserved.

**The mock callback layer cannot be used to drive real hardware.** It has two compile conditions, `PJ_BRINGUP_TEST_CALLBACKS` and `PJ_BRINGUP_HOST_TEST`, with an additional deliberate compilation error if kernel headers or `_KERNEL_MODE` are present. Production calls return `PJ_BRINGUP_LOCKED` without invoking any callback. Neither module is added to the Windows project files.

## Requirements for a *real* Windows path (not yet implemented)

1. **Firmware:** A compatible, legitimately distributed **Alder Lake-N** SOF IPC4 image (`sof-adl-n.ri` alone does not prove compatibility), verified layout, signature, module/table format, device-specific ROM/DSP boot and IPC4-ready/capability handshake. Full DMA/interrupt/power-error handling must be reviewed independently. See [SOF project boot architecture](https://thesofproject.github.io/latest/developer_guides/firmware/fw_init_boot.html).
2. **Audio pipeline:** A Windows IPC4 topology/DAI/SSP1 graph matching Pujjo's physical wiring (not the Linux ALSA `.tplg` used as a candidate reference). Real clock, sample format, SSP ownership, audio endpoint and Windows PortCls mixer/volume controls.
3. **RT1019P amplifier:** PnP ownership for ACPI `RTL1019`, validated translated GPIO I/O Resource Hub connection; verified physical logical polarity and electrical safe state; muted startup, first-time calibration, reference-count/power lifecycle and safe shutdown. Use `IOCTL_GPIO_WRITE_PINS` only with the PnP-assigned connection and a correctly sized mask, not a hard-coded ACPI pin number. [Microsoft GPIO resource guide](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/gpio-based-hardware-resources).
4. **RT5682S codec:** PnP ownership for `RTL5682`, validated SPB I2C connection, correctly serialized and cancellable transactions, actual register configuration/calibration/PLL and jack path. Atomic register-address write / register-value read requires `IOCTL_SPB_EXECUTE_SEQUENCE` and correct `SPB_TRANSFER_LIST`; mock identity reads do not initialize the codec. [Microsoft SPB guide](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/using-the-spb-transfer-list-structure).
5. **Orchestration and safety:** At D0, validate connection ownership, mute and safe gain limits, load/verify firmware, configure SSP1, keep amplifier disabled until DSP and stream readiness, then carefully enable and unmute. On D3, removal, errors or power loss, stop new requests, drain/cancel in-flight IO, mute and disable the amplifier. Only real Pujjo hardware tests can establish electrical conditions.
6. **Windows build/deployment:** MSBuild x64 Windows driver solution with WDK 26100, driver signing and correct INF/CAT ownership, Driver Verifier/analysis, then controlled tests on a recoverable target system. No steps to install experimental drivers are included here.

### Important hardware mapping

- PCI: `8086:54C8`, Google Pujjo / Pujjo1e only.
- Headset codec: `RTL5682` = RT5682S, likely SSP0, I2C3 `0x1A` from firmware evidence.
- Speaker amplifier: `RTL1019`, often discussed as RT1019P; the Linux codec implementation for this ACPI ID is **`rt1015p.c`**, not the separate `rt1019.c` file. Linux candidate speaker route is SSP1 and SDB is a GPIO resource (ACPI describes pin 75). These are **not** verified Windows connection IDs or proof of output polarity.

### How to run hardware-free checks

From the extracted project root with Python 3 and a C compiler:

```
python tests/run_bringup_tests.py
python tests/test_bringup_static.py
python tests/run_offline_suite.py
```

Compiler outcomes are host-only. A pass does not mean WDK code compiles. No audio activation or installation is authorized by these tests.

### Source licensing

Retain the upstream BSD-3-Clause license. No proprietary CoolStar code, binary or activation code has been added or altered. GPL Linux kernel source was consulted solely for behavioral references; none was copied into the new files.
