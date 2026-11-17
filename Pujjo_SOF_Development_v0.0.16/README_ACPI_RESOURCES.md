# Pujjo rev3 raw ACPI evidence — v0.0.8

This is **read-only** firmware evidence collected from the device's actual firmware tables on Windows 11. The SSDT raw AML was parsed without evaluating ACPI methods. An ACPICA disassembler could not process the SSDT because of a repeated `CNMT` namespace object; a static parser was used to extract literal `_CRS` ResourceTemplate buffers associated with the two Realtek hardware IDs.

## Verified descriptors

| Device | Resource | Decoded value |
|---|---|---|
| `RTL5682` (`PCI0.I2C3.RT58`) | I²C serial bus | `\\_SB.PCI0.I2C3` at **0x1A** (7-bit), **400,000 Hz** |
| `RTL5682` | GPIO interrupt | `\\_SB.PCI0.GPIO`, pin **87** |
| `RTL5682` | `_DSD` properties present | `irq-gpios`, `realtek,jd-src` |
| `RTL1019` (`PCI0.I2C3.ALCP`) | GPIO output | `\\_SB.PCI0.GPIO`, pin **75** |
| `RTL1019` | `_DSD` property present | `sdb-gpios` |
| `RTL1019` | I²C resource | **None advertised in its `_CRS`** |

**Caution:** Although `ALCP` lives under the `I2C3` ACPI namespace, it exposes a **GPIO output** for the RT1015P-style amplifier, not an I²C connection. Windows PnP placement alone must not be mistaken for a bus address. A PnP node's existence does not prove its power sequence or pin-level behavior.

## What was added

- `tools/analyze_acpi_audio.py`: optional offline parser, requiring the user's own **raw SSDT `.dat`** file. No ACPI table or device-specific private dump is bundled.
- `src/driver/experimental/PujjoAcpiResources.[ch]`: portable validation of **expected** ACPI resources; deliberately **not linked** into the Windows kernel driver.
- `tests/run_acpi_resource_tests.py` and `tests/test_acpi_parser.py`: mock/static tests. No hardware access.

Example after providing your own `SSDT.dat`:

```bash
python tools/analyze_acpi_audio.py --ssdt /path/to/SSDT.dat
python tests/test_acpi_parser.py
python tests/run_acpi_resource_tests.py
```

## Still missing / safety

- Windows KMDF SPB connection resource capture and GPIO I/O target resource acquisition.
- Confirming runtime resource assignment, interrupt polarity, GPIO logical activation behavior, and power sequencing in Windows.
- SOF firmware compatibility, **SSP1** audio topology, amp speaker-safe gain limits, and driver build under WDK.
- This is **not** a functional speaker driver. It must not be installed; no amplifier/codec register writes are provided.

The raw GPIO descriptor flags are kept in the JSON output rather than interpreted as a command to drive the physical pin. The ACPI table alone does not authorize powering the speaker.
