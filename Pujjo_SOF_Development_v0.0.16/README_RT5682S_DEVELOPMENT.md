> **Current release: v0.0.14 — SOURCE ONLY, NOT INSTALLABLE.** See [CODEX_RESULTS.md](CODEX_RESULTS.md), [CHANGES.md](CHANGES.md) and [REMAINING_BLOCKERS.md](REMAINING_BLOCKERS.md). Run `python tests/run_offline_suite.py` for the full offline allowlist. Windows WDK compilation remains unverified.

> The material below is retained release history; earlier version numbers and test counts describe those earlier packages, not v0.0.14.

# RT5682S codec work – Pujjo source v0.0.11 (2026-11-14)

**Development code only; do not install on the Lenovo.** No functional audio
codec, headset, microphone, or speaker endpoint has been demonstrated.

## Verified chipset facts (external references)

- The `RTL5682` ACPI HID can describe the **RT5682S/RT5682I-VS** device.
  Do **not** substitute Linux's separate `rt5682.c` programming for `rt5682s.c`.
- The public Linux RT5682S implementation uses 16-bit register addresses,
  16-bit register data, `0x00FF` as chip-ID register and `0x6749` as chip-ID
  value. Bus register reads need a register-address write followed by a read
  in one atomic I2C sequence; register writes are 2-byte address + 2-byte data.
- The user's raw Pujjo SSDT shows the device under `PCI0.I2C3.RT58` with
  7-bit I2C address `0x1A`, 400 kHz and separate GPIO jack interrupt 87.
  The Windows driver must open the *assigned Resource Hub connection ID*, not
  manually use address 0x1A or program I2C controller registers.
- Realtek exposes analog calibration, power rail requirements, clocking/PLL,
  DAI, jack detection and pop suppression. A device-ID match alone does NOT
  initialize those functions.
- The RT1019P speaker amplifier is **separate** from RT5682S, and reports a
  GPIO connection (pin 75) rather than another I2C codec connection.

## Implemented in v0.0.11

| Path | New components | Default mode |
|---|---|---|
| `PujjoRt5682sCodec.[ch]` | Portable codec identity/encoding/state, read guard, bus error handling, mock-only register RMW | Read-only |
| `PujjoRt5682sSpbWindows.[ch]` | Windows KMDF Resource Hub target and atomic SPB read / 4-byte write callbacks, 1s timeout, close | Compiled but unopened |
| `PujjoPeripheralKmdf.c` | **Opt-in-only** path opening SPB after SMBIOS/ACPI/PnP resource checks; read ID after entering D0; sleep/wake revokes validation | Opt-in macro *NOT defined* |
| `tests/test_rt5682s_codec.c` | Simulated register bus: identification, exact bytes, read-allowed registers, wrong chip, errors, suspended bus, mock-only writes | Host only |
| `tests/run_rt5682s_codec_tests.py` | Compiles/runs release-locked and explicitly mock-write-enabled variants; skips with notice if no local compiler | Host only |
| `tests/test_rt5682s_static.py` | Checks that default PnP implementation cannot open bus; checks Windows transport sources and disabled write policy | Host only |

`PJ5682S_TEST_ONLY_WRITES` compiles a **mock-only** bit-update test path,
not a production device feature. NEVER enable it on a physical machine.
All normal builds return `PJ5682S_WRITE_LOCKED` for attempted codec writes.

The KMDF code has been reviewed against Microsoft documentation but has NOT
been built with the Windows WDK. Any API/signature or toolset errors still need
correction on a Windows runner.

## Very important research find: an existing Windows codec driver

There is already **public Windows RT5682S source** from CoolStar:
https://github.com/coolstar/rt5682s

Its README reports support for jack detection, headphones, microphone, and
sleep/wake. It was tested on the Framework Laptop Chromebook Edition, **not
on this Pujjo**. The repo carries Apache-2.0 for CoolStar's work and MS-PL
for Microsoft-sample portions: inspect every copied file's attribution and
license before adapting code.

**Correction to an earlier claim**: a particular packaged CoolStar archive
contained binaries, but independent public RT5682S Windows driver **source**
does exist. This is the best available starting point for the *missing
full codec logic*, rather than reimplementing unverified analog calibration
and jack detection from memory. Its README explicitly warns that a compatible
SOF/SST/ACP audio controller layer is also required: the open Windows codec
driver alone will not enable sound through the experimental Pujjo SOF fork.
The vendor project has not been imported into this BSD-3-Clause source fork.
No GPL Linux kernel or CoolStar Windows code was copied into these new files.

## Still missing (NOT implemented or validated)

1. A tested Windows WDK compilation and reproducible CI build.
2. Vendor-accurate power-supply startup, reset, analog calibration and pop
   suppression, DAI format, PLL/sysclk, bias level, headset jack detection
   and microphone routing (the proposed external Windows driver is best
   reference).
3. Binding/coordination between this Windows ACPI codec child and the Pujjo
   Intel N100 SOF PCI DSP driver; Windows audio topology, stream and endpoint
   management. Candidate SSP0/SSP1 associations are not proven on Windows.
4. Safely controlling D0/D3 suspend/resume with real regulator sequencing,
   interrupt handling, _DSD jack properties, audio volume limits, and rollback.
5. Speaker power/mute is a *separate* RT1019P and DSP routing problem.
6. Safe driver packaging/signing, Driver Verifier/HLK, hardware playback,
   microphone and speaker verification on a recoverable system.

**Next best development path:** build and audit the public Windows RT5682S
code *alongside* the SOF PCI DSP project on a hosted Windows+WDK build
runner, preserve source licenses, and define a compatible topology/codec
interface. Only consider hardware tests when the end-to-end code, signing,
recovery, and minimal-volume path have been independently reviewed.

## References

- Linux `rt5682s.c` (GPL-2.0-only, for hardware behavior facts only):
  https://github.com/torvalds/linux/blob/master/sound/soc/codecs/rt5682s.c
- Linux `rt5682s.h` (register addresses/facts):
  https://android.googlesource.com/kernel/common/+/16c7a368c8ba9aba3b0e0ae5ec0a64be9fb45665/sound/soc/codecs/rt5682s.h
- Windows RT5682S project and license: https://github.com/coolstar/rt5682s
- MS SPB connection ID handling:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/connection-ids-for-spb-connected-peripheral-devices
- Atomic SPB read sequences:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/using-the-spb-transfer-list-structure
- KMDF power lifecycle:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/wdm-irps-and-kmdf-event-callback-functions
- Driver signing/installation:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/install/pnp-device-installation-signing-requirements--windows-vista-and-later-
