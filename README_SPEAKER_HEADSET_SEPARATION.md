# v0.0.6: Separate speaker and headset paths (no working driver yet)

## What the supplied Linux sources establish

The `chromebook-linux-audio` Alder Lake binary topology
`sof-adl-rt1019-rt5682.tplg` names `PIPELINE.1.SSP1.OUT` / `SSP1-Codec`
(speaker candidate) and `PIPELINE.2.SSP0.OUT` / `SSP0-Codec`
(headset candidate). `SSP2-BT` belongs to the digital BT pipeline. This is
strong *reference-topology evidence*, **not** proof the Pujjo EC/ACPI firmware
has exactly this physical wiring on Windows.

The official Linux kernel sources in the user-provided `linux-master.zip`:

- `sound/soc/codecs/rt1015p.c`: ACPI `RTL1019` uses the RT1015P family,
  with an optional `sdb` GPIO (not an ordinary I2C register-programmed amp);
  Linux models a 300 ms delay on the first enable, and supports 48 kHz
  24/32-bit playback. This is Linux GPL-2.0-only reference behavior.
- `sound/soc/codecs/rt5682s.c`: `RTL5682` codec, I2C, clock/DAI configuration;
  belongs to the *headset path*, not the SSP1 speaker amplifier path.
- `sound/soc/intel/boards/sof_rt5682.c`: board glue binds codec and amp
  to Intel DSP SSP pipelines. RT5682S clock selection depends on actual
  input MCLK/BCLK and the chosen PCM parameters.

## Corrections made

Earlier v0.0.4 code configured RT5682S while preparing speaker SSP1. That
mixes up the independent codec and amplifier roles. v0.0.6 changes the
experimental speaker state machine to **only** use abstract DSP speaker
mute, SSP1 link setup and abstract amplifier SDB enable/disable. There is
no RT5682S register access in this speaker code. The amplifier cannot
be reliably 'probed' via I2C; ACPI presence does not establish readiness.

The simulated activation sequence is amp off -> mute speaker DSP path ->
configure SSP1 -> amp on -> 300ms candidate delay -> DSP unmute, with
best-effort mute/amp-off rollback on failure. Every true hardware action
remains UNIMPLEMENTED; this runs only with the opt-in mock test.

The speaker gate now *also* requires independently validated speaker
level limiting. The Linux project warns that uncontrolled amplifier
levels can harm speakers on some Chromebook models. No limiter is
currently implemented, so the gate must stay false on real hardware.

## Additional code

`PujjoHeadsetClock.[ch]` calculates candidate format and clocks for the
*separate* RT5682S/SSP0 headset link. With 48 kHz, two channels and
24-bit samples, 32-bit slots imply BCLK = 3.072 MHz and SYSCLK target
= 24.576 MHz. 24-bit slots instead imply 2.304 MHz BCLK. **These
numbers are arithmetic candidates, not verified platform settings.**
It refuses to assume an MCLK/BCLK source frequency and does not generate
PLL registers or I2C traffic. The RT5682S clock planner is NOT linked
into the kernel driver.

`scripts/read_only_acpi_audio_evidence.ps1` collects Windows PnP and DMI
properties for `54C8`, `RTL5682`, and `RTL1019`, never touches actual
hardware control resources and makes no readiness claims.

## Test (on a development computer, not a kernel install)

```sh
python tests/run_codec_init_tests.py
python tests/run_headset_clock_tests.py
python tests/run_topology_evidence_tests.py /path/to/chromebook-linux-audio-main.zip
```

**No Windows WDK compilation or speaker playback has been performed.**
No actual WDF/SPB I2C client, ACPI resource parser, GPIO connection,
DSP mixer/limiter, or firmware IPC implementation is supplied. The original
Pujjo install safeguard remains active. Do not run install scripts or
experimentally power the speakers on a primary Windows laptop.

License boundaries: forked SOF Windows base is BSD-3-Clause; Linux
kernel files are GPL-2.0-only references, NOT included or copied;
CoolStar's proprietary binaries are not incorporated; the Linux project
BSD license applies to its own code, not all downloaded firmware blobs.
