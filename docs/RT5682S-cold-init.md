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
