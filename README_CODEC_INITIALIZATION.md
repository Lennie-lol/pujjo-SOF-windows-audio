# Codec and speaker initialization — v0.0.6 status

**Source-only development experiments, NOT a functioning Windows audio driver.**
The original v0.0.4 mock incorrectly combined RT5682S headset initialization with
RT1019P speaker playback. This was fixed in **v0.0.6**.

Read [`README_SPEAKER_HEADSET_SEPARATION.md`](README_SPEAKER_HEADSET_SEPARATION.md)
for the updated design, Linux references, safe tests, and blockers.

- `src/driver/experimental/PujjoAudioInit.[ch]`: SSP1 **speaker** coordinator,
  which uses only a hypothetical SOF speaker mute, SSP1 link, and abstract
  RT1019P amp enable. It has **no register writes or real WDF backend**.
- `src/driver/experimental/PujjoHeadsetClock.[ch]`: separate **SSP0/RT5682S**
  headset clock-format planning, with unknown physical clock sources rejected.
- Default compile refuses audio I/O; optional compile flag is for simulated
  unit-test callbacks only. Neither file is linked into the real driver.
- `scripts/read_only_acpi_audio_evidence.ps1` provides non-invasive Windows
  inventory. This does not reveal physical SSP routing, amp polarity or gain.

Tests:

```sh
python tests/run_codec_init_tests.py
python tests/run_headset_clock_tests.py
python tests/run_pujjo_tests.py
```

Licensing: The code written in these experimental modules is BSD-3-Clause.
The Linux kernel files studied as references are GPL-2.0-only, and no
GPL or proprietary driver implementation was copied into the modules.

**Do not install this fork or enable amplifier power on your laptop.**
