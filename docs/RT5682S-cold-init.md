# RT5682S cold initialization candidate

Reference: Linux v6.13 RT5682S register definitions and probe/calibration behavior:
https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt5682s.c
and https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt5682s.h.

The portable control engine now has a separate cold-power-epoch state machine.
It is intentionally not called by the live Windows route code. The owner must
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

Next: validate rails/LDO ownership and reset timing, capture calibration busy
transition and readbacks, and design a power-epoch owner integration (D0/D3
invalidation and route gating). Do not enable device writes based on host tests.

Power-epoch integration: the optional Windows SPB control owner now always
invalidates cold READY on resource open, D0 exit and close; start and jack paths
require READY and are refused by default. A pending register rollback on D0
re-entry is latched as a fault rather than replayed against an unknown codec
state. The cold-init executor is *not yet wired* to an actual Windows write
transport: no rail/LDO/reset electrical qualification has been established, so
no activation or installed-driver functionality is being claimed.

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
