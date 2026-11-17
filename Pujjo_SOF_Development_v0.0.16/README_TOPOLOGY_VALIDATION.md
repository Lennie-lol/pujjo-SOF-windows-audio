> **v0.0.6 update:** See `README_SPEAKER_HEADSET_SEPARATION.md` for the corrected separation of SSP0 headset codec and SSP1 speaker amp.

# Pujjo Alder Lake topology validation (v0.0.5, retained in v0.0.6)

**This is not a working speaker driver. No kernel driver was built, signed or run.**

## New code

`src/driver/experimental/PujjoTopologyEvidence.[ch]` is a standalone, portable,
BSD-3-Clause C parser that validates the outer ALSA topology container headers
(36-byte `CoSA` header sections) and extracts bounded, NUL-terminated route
markers. Its job is to reject an obviously wrong or damaged *candidate* SOF
Linux topology blob before development proceeds.

The parser checks for `SSP1.OUT` / `SSP1-Codec` (candidate speakers),
`SSP0.OUT` and `SSP0.IN` / `SSP0-Codec` (headset), and `SSP2.OUT` and
`SSP2.IN` / `SSP2-BT` (digital Bluetooth audio). It does not parse ALSA
widgets, DSP IPC3 vs IPC4 configuration, clock values, actual Pujjo hardware
connections or speaker gain limits. **Marker presence is not proof of a
working Windows firmware topology.** It should never set the existing
`ssp1_routing_tested` verification flag by itself.

The original binary `.tplg` and GPL Linux kernel sources are **not redistributed**
in this ZIP. The parser is independent code against publicly documented
container structures; no copyrighted GPL source or proprietary CoolStar
binaries have been copied.

## Real-archive test

Given the user-supplied `chromebook-linux-audio-main.zip`, run on a developer
computer with a C compiler and Python:

```bash
python tests/run_topology_evidence_tests.py /path/to/chromebook-linux-audio-main.zip
python tests/run_codec_init_tests.py
python tests/run_pujjo_tests.py
```

The new test extracts the user's Linux reference topology to a temporary
directory, compiles the C parser, and verifies the 31 ALSA topology sections
and expected route strings. It then rejects a damaged header, truncated
contents, invalid input, and a topology with the SSP1 speaker route removed.
No firmware or real hardware I/O is performed.

## Observations and remaining blockers

- Linux script maps `ACPI\\RTL5682` to `SND_SOC_RT5682S`.
- Linux script maps `ACPI\\RTL1019` to `SND_SOC_RT1015P`, which includes the
  RT1019-compatible amplifier; unlike the codec, this amplifier is largely
  managed through a GPIO shutdown/enable signal in Linux.
- Linux RT5682 machine-driver logic configures clocks and a codec DAI; this
  logic **cannot be directly pasted** into the BSD Windows project because the
  source files are GPL-2.0-only and target ALSA/ASoC.
- SSP1 is a *candidate* physical speaker route. We still lack Pujjo-specific
  ACPI `_CRS` resource mapping, explicit SSP link clocks and TDM slot width,
  working Windows RT5682S I2C/RT1019 GPIO driver backends, firmware protocol
  compatibility, and measured speaker power/volume limits.
- The Windows SOF source has IPC4-focused firmware loading and topology
  creation; the Linux `.tplg` format/graph is **not a drop-in Windows IPC4
  topology**. Firmware/IPC version must be independently verified.

## Safe next test on the actual Lenovo

Only the existing read-only `scripts/read_only_codec_preflight.ps1` or
`scripts/read_only_pujjo_check.ps1`. These verify PnP/SMBIOS presence, **not**
backend readiness. Do not install the experimental fork or attempt speaker
power writes on the primary Windows installation.
