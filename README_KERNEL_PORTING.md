# Pujjo / Pujjo1e kernel-source mapping (v0.0.3)

**Research only. No operational speaker path has been added. Do not install this driver.**

Reference supplied: `linux-master.zip` from `https://github.com/torvalds/linux`,
which self-reports `VERSION=7`, `PATCHLEVEL=4`, `EXTRAVERSION=-rc2` in its
`Makefile` at the time of analysis. This is an **unreleased release-candidate
snapshot**, not a stable kernel release. No commit hash was embedded in the ZIP.

## Device-to-source mapping (verified in user-supplied archive)

| Windows device ID | Linux source | What it establishes |
| --- | --- | --- |
| `PCI\VEN_8086&DEV_54C8` | `sound/soc/sof/intel/pci-tgl.c` | Intel ADL-N PCI controller maps to the SOF ADL-N platform description |
| `ACPI\RTL5682` | `sound/soc/codecs/rt5682s.c` | Realtek RT5682S codec implementation; codec requires clock/PLL, I2C register and signal-path configuration |
| `ACPI\RTL1019` | `sound/soc/codecs/rt1015p.c` | `RTL1019` is an explicit ACPI alias of this driver, although the chip is described as RT1019P |
| Both Realtek IDs | `sound/soc/intel/common/soc-acpi-intel-adl-match.c` | ADL machine-driver selection by codec combination, with topology filename selection |
| Codec+amp links | `sound/soc/intel/boards/sof_rt5682.c` | Machine-driver DAI, jack, clock, and amplifier link setup |

### Key correction: RTL1019 vs RT1019

`RTL1019` (as reported in Windows) maps to `rt1015p.c` in the kernel. This
**does not** mean we should use the *separate* `rt1019.c`, which represents
another chip/driver. The Linux userspace script also maps `RTL1019` to
`rt1019p` and selects Kconfig `SND_SOC_RT1015P`.

### Reference power behavior (not implemented)

The `rt1015p.c` implementation obtains an optional `sdb` GPIO and uses it
for enable/disable transitions. The Linux implementation waits for amplifier
calibration on first power-up. Windows must implement proper GPIO resources,
power management and conservative mute/speaker-volume handling **before**
any output is enabled; do not take this as an instruction to toggle an
unidentified GPIO directly.

### Topology in separately supplied Linux audio ZIP

`chromebook-linux-audio-main.zip` includes
`blobs/adl/sof-adl-rt1019-rt5682.tplg`, whose named graph indicates:

- `SSP1.OUT` — speakers (reference only)
- `SSP0.OUT` / `SSP0.IN` — headset path
- `SSP2.OUT` / `SSP2.IN` — digital BT link (not Windows Bluetooth headphones)

Those labels are **evidence of the Linux graph**, not proof of Pujjo's
physical routing or of a usable Windows IPC4 firmware topology.

## Explicit Windows porting tasks (not yet complete)

1. Confirm Pujjo/Pujjo1e ACPI/NHLT connections and exact board quirks.
2. Confirm *firmware format and IPC protocol* expected by this Windows fork.
3. Implement Windows access to RT5682S and RT1019P devices through actual
   enumerated ACPI/I2C/GPIO resources (not arbitrary address guesses).
4. Implement clocks, valid I2S stream format, volume/mute sequencing, codec
   power-up/down and first-activation delay based on trusted documentation.
5. Replace the current disabled speaker endpoint with a working, validated
   PortCls/WaveRT path only after earlier steps are done.
6. WDK compile, signing, restart/recovery testing, and hardware validation.

### License boundary

The Linux kernel source files are marked `GPL-2.0-only` or other applicable
SPDX identifiers. They are excellent technical references, but this **does
not permit relabeling copied Linux code as BSD-3-Clause**. For now this fork
contains only our own factual mapping and tests, not the GPL-licensed Linux
codec driver source or any CoolStar compiled binaries. Any future reuse of
GPL-covered code would require compatible licensing and preservation of
notices, alongside a full license audit of the Windows project's dependencies.

## Reproducible, read-only verification

From the fork's root, with Python 3:

```powershell
python tests\verify_kernel_reference.py "C:\path\to\linux-master.zip"
```

This script reads ZIP members, validates the facts above, and makes **no**
hardware or Windows configuration changes.

## Status

| Milestone | Status |
| --- | --- |
| Exact component IDs | Confirmed on user's Pujjo |
| Linux driver references | Confirmed |
| Linux SSP topology labels | Inspected, not Pujjo-hardware-verified |
| Windows codec implementation | Missing |
| Windows SOF speaker playback | Explicitly disabled |
| Windows WDK compile | Not performed |
| Windows speaker hardware test | Not performed |
