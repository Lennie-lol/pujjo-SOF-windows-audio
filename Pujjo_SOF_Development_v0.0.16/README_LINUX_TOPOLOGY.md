# Linux SOF topology reference for Pujjo / Pujjo1e (v0.0.2)

**This is not a working Windows speaker driver. Do NOT install this fork.**

## New input

Inspected the user-supplied `chromebook-linux-audio-main.zip` from
[WeirdTreeThing/chromebook-linux-audio](https://github.com/WeirdTreeThing/chromebook-linux-audio).

This project is a **Linux setup script and selected firmware/topology assets**;
it is not a Windows driver, and the binary `.tplg` file is not source code.

The script treats `google_nissa` (Intel Alder Lake-N) as platform `adl`. Its
`adl_sof_config()` installs a replacement topology
`blobs/adl/sof-adl-rt1019-rt5682.tplg`, with the comment that the
upstream `sof-adl-rt1019-rt5682` topology is currently broken.

`setup-audio` also maps:
- `RTL5682` -> `rt5682s` -> Linux `SND_SOC_RT5682S`
- `RTL1019` -> `rt1019p` -> Linux `SND_SOC_RT1015P`

These correspond to the **actual ACPI IDs observed** on the user's Pujjo rev3.

## Evidence from the uploaded `.tplg` (55,343 bytes)

The file includes ALSA topology graph names and endpoints:

| Linux pipeline label | Endpoint | Interpretation |
|---|---|---|
| `PCM0P`, `Speakers 0`, `PIPELINE.1.SSP1.OUT` | `SSP1.OUT` / `SSP1-Codec` | Candidate for internal speaker output |
| `PCM1P`, `PCM1C`, `Headset 1` | `SSP0.OUT`, `SSP0.IN` / `SSP0-Codec` | Headset playback and capture |
| `PCM7P`, `PCM7C`, `Bluetooth 7` | `SSP2.OUT`, `SSP2.IN` / `SSP2-BT` | Digital Bluetooth audio interface, **not the Windows Bluetooth headset path** |
| `PCM99C`, `PCM100C` | `DMIC0.IN`, `DMIC1.IN` | Digital microphones |

**Caveat:** The topology is a model for the ADL RT1019/RT5682 combination;
we have *not* proven the firmware loads exactly this configuration on either
Pujjo or Pujjo1e, nor have we checked the full clock/slot/amp sequence.

## Windows port implications

The forked Windows source currently implements **Intel SOF IPC4** operations.
The uploaded `.tplg` is an **ALSA/SOF Linux topology blob** and cannot simply
be copied into the Windows WDK driver or treated as a ready-to-load IPC4
pipeline. Its graph and configuration must be interpreted, checked against
the actual Windows DSP firmware protocol and translated to proper driver
initialization and Windows audio endpoints.

The upstream Windows SSP2 path was generic and is **not** evidence that SSP2
drives Pujjo's speakers. In this fork speaker playback remains fail-closed via
`STATUS_NOT_SUPPORTED`; this update adds *reference-only* constants in
`src/driver/PujjoLinuxTopology.h`.

Before enabling any speakers:
1. Confirm the exact DMI/ACPI topology for Pujjo and Pujjo1e individually.
2. Confirm SOF firmware IPC protocol and compatible independently licensed firmware.
3. Implement RT5682S and RT1019P codec/power sequencing and link format safely.
4. Validate speaker volume limits, mute, suspend/resume and failure paths
   in a recoverable test environment, prior to hardware playback tests.
5. Build and sign the Windows driver with WDK, then test thoroughly.

## Licensing boundary

The base Windows code is BSD-3-Clause, and the Linux script includes
BSD-3-Clause licensing text. This fork does not copy CoolStar licensed
binaries and **does not redistribute the Linux `.tplg` binary**, whose own
redistribution provenance was not independently verified.

## Software-only checks

From the project root:

```sh
python3 tests/verify_linux_topology.py /path/to/chromebook-linux-audio-main.zip
python3 tests/run_pujjo_tests.py
```

No driver installation or audio playback occurs.
