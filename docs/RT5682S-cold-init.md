# RT5682S cold initialization candidate

Reference: Linux v6.13 RT5682S register definitions and probe/calibration behavior:
https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt5682s.c
and https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt5682s.h.

The portable control engine now has a separate cold-power-epoch state machine.
Production Windows routes do not invoke it. An opt-in lab-only SPB bridge
exists but has no PnP caller or qualified board power owner. The owner must
establish rail timing, exclusive codec control, output isolation and serialized
power/removal before invoking it. The successful sequence is not a board
qualification, and no existing installation policy is changed.

The reference uses a bounded busy-bit poll; this candidate additionally requires
observing a busy-to-idle transition, which can reject a device that finishes before
the first observation. A 1 ms post-reset pause is conservative but not board
measured. Full-width stable readback assumptions need DUT validation. Volatile
commands are not read back as ordinary registers. A bus failure after reset
causes an identity-checked best-effort reset, a second identity check after the reset, and a latched fault; the external
isolation must remain asserted even if recovery succeeds. No arbitrary reset
rollback or in-place retry is allowed.

Next: independently validate rails/LDO ownership and reset timing, capture
calibration busy transition and readbacks, and implement a board-specific power
owner that can prove a genuine new power epoch. D0/D3 state invalidation and
route gating are software-integrated; they are not electrical qualification.

Power-epoch integration: the optional Windows SPB control owner now always
invalidates cold READY on resource open, D0 exit and close; start and jack paths
require READY and are refused by default. A pending register rollback on D0
re-entry is latched as a fault rather than replayed against an unknown codec
state. The cold-init executor is connected to an opt-in **lab-only** Windows SPB
write transport. That is deliberately *not* enabled or called in the production
build: rail/LDO/reset electrical qualification remains missing, and neither
activation nor installed-driver functionality is being claimed.

## Windows SPB cold bridge (lab builds only; 2026-11-21)

The optional `Pj5682sSpbColdInitialize` now connects the portable cold
executor to the existing synchronous Windows Resource Hub SPB read/write
transport under the owner's wait lock, so D0/D3, close, control routes and
calibration cannot interleave. Only registers used by the audited reset and
calibration program can be accessed by the private cold path; diagnostic bus
callbacks remain strictly read-only.

**Production builds remain disabled.** The source-only WDK project defines
neither `PJ5682S_ENABLE_CONTROL` nor
`PJ5682S_ENABLE_LAB_COLD_INIT`. Without the lab macro, the bridge returns
`STATUS_ACCESS_DENIED` before touching I²C. There is no PnP caller.

For controlled lab builds only, the board owner must independently verify and
affirm **all** of these at the moment of entry:
- A genuinely fresh codec power/reset epoch, not simply Windows entering D0.
- Board identity, ownership of the correct translated I²C connection, clock
  and rails/LDO settling for Pujjo/Pujjo1e.
- Output isolation/mute, and exclusive codec control across reset and calibration.
- Absence of active streams, saved-register rollback or latched control faults.

The lab transport permits one cold attempt per D0 epoch. It records READY only
after the portable state machine succeeds; on transfer failure it latches a
fault, clears the private transport capability and refuses in-place retries.
The lab bridge cannot prove the reported qualification claims or physically
isolate the amplifier by itself. Those must be established by a board-specific
Windows power owner and independent hardware evidence. A new D0 is NOT a
verified power-cycle or permission to clear a latched fault.

`tests/run_windows_transport_tests.py` compiles and runs both production-
disabled and opt-in host-lab modes with a simulated codec reset, calibration
busy/idle status and error injection. This demonstrates software behavior,
**not** electrical qualification or working native audio. Do not install the
candidate on the Chromebook or infer speaker/microphone function from host CI.

## Pujjo rev3 evidence inventory: established facts versus open checks

The earlier owner-supplied Windows PnP inventory and offline ACPI extraction
established these **descriptive** facts (see historical consolidated Pujjo
evidence; none is an authorization to access registers or assert READY):

| Item | Read-only evidence | Qualification limit |
| --- | --- | --- |
| Baseboard | Google Pujjo rev3 | Pujjo1e is not separately measured |
| Codec | `ACPI\\RTL5682`, `PCI0.I2C3.RT58`, Intel I²C parent `8086:54EB` | Runtime translated SPB connection and exclusive owner unverified |
| ACPI codec connection | Address `0x1A`, 400 kHz in static `_CRS` | Static address/rate must not be used as Windows Resource Hub IDs |
| Jack interrupt | ACPI GPIO controller, raw pin 87 | Interrupt trigger, routing and runtime resource validation missing |
| Speaker amplifier | `ACPI\\RTL1019`, `PCI0.I2C3.ALCP` | Matches Linux RT1015P-family shutdown GPIO, **not** an I²C amp |
| Amplifier shutdown | Raw ACPI GPIO output pin 75, `sdb-gpios` | Active polarity and safe isolation state unmeasured |
| DSP paths | Candidate SSP0 codec / SSP1 speaker and DMIC labels in Linux topology | Windows link clock, pinmux and pipeline operation unverified |

Outstanding evidence before *any* lab cold-init hardware experiment:
1. Establish codec rail/LDO supply ownership, the real power-off-to-on
   boundary, and reset duration under Pujjo-specific firmware/EC behavior.
2. Verify correct Windows translated I²C connection and independent exclusive
   control over the codec while DSP routes are muted.
3. Determine MCLK/BCLK/FSYNC ownership and actual frequencies, SSP pinmux and
   relevant clock/reset settling; Linux candidate frequencies are not measurements.
4. Establish amplifier SDB shutdown polarity and actual output isolation before
   codec reset, including D0→D3 and failure cases.
5. Verify actual calibration status transitions and readbacks on a qualified
   development device with a recoverable debug path.

An existing ChromeOS recovery image supplies firmware/topology *reference*
data, but cannot by itself prove the live power sequence, GPIO polarity or
Windows ownership. A later read-only Linux dmesg capture is useful but is not
mandatory to continue offline work.

## Optional offline Linux boot-log triage

`scripts/analyze_pujjo_audio_boot_log.py` takes a previously saved Linux
`dmesg` text file; it does **not** read hardware, run a driver or need Linux
at analysis time. Example once a log exists:

```text
python scripts/analyze_pujjo_audio_boot_log.py path/to/dmesg.txt --json
```

The output provides line numbers and time order for SOF, codec, amplifier,
SSP and DMIC clues, plus possible failure lines. It never marks the codec
qualified or authorizes writes. Host tests in
`tests/test_rt5682s_boot_evidence.py` cover recognized, missing and
error-bearing log input. Existing `tests/test_rt5682s_static.py` invokes
that regression. No new log is needed to run the tests.
