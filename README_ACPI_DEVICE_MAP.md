# Pujjo/Pujjo1e read-only ACPI device-map evidence (v0.0.7)

## From actual Pujjo rev3 Windows inventory

| Hardware | Windows PnP | ACPI location | Parent |
|---|---|---|---|
| Intel SOF controller | `PCI\\VEN_8086&DEV_54C8...` | `PCI0.HDAS` | PCI root |
| RT5682S/RTL5682 | `ACPI\\RTL5682\\0` | `PCI0.I2C3.RT58` | Intel `8086:54EB` |
| RT1019P/RTL1019 | `ACPI\\RTL1019\\0` | `PCI0.I2C3.ALCP` | Intel `8086:54EB` |

This evidence comes from an actual Windows 11 read-only PnP diagnostic supplied by the Pujjo owner. Both Realtek devices share an enumerated PCI I2C3 controller parent. **This does not mean they share the same I2C slave address, or prove that RT1019P is directly controlled through I2C.** In the Linux kernel, the `RTL1019` device matches the RT1015P-style driver whose shutdown GPIO is optional.

## New code

- `src/driver/experimental/PujjoDeviceMap.[ch]`: portable C, fail-closed identity/parent/path validation; no WDF or hardware access, not linked into kernel driver.
- `tests/test_pujjo_device_map.c` / `tests/run_device_map_tests.py`: 10 checks including mismatches and missing evidence.
- `scripts/export_pujjo_audio_inventory.ps1`: safely exports structured board/PnP data for comparison without modifying hardware or installing a driver.

## Still unverified

1. **ACPI `_CRS` / `_DSD` resource descriptors** for `RT58`, `ALCP`, and `HDAS` (I2C connection IDs, potential GPIO connections, clock resources).
2. Confirm hardware's SOF **SSP0 / SSP1** wiring; Linux topology suggests, but Windows PnP data cannot prove it.
3. Speaker-safe volume limits, amplifier shutdown GPIO polarity, and recovery handling.
4. Working SOF firmware loading, endpoints, and codec initialization under Windows.

Do **not** install this project as a driver or run unverified GPIO/amplifier writes. A C test passing is not a Windows hardware test.

## Safe command

Open PowerShell in the project directory:

```powershell
.\\scripts\\export_pujjo_audio_inventory.ps1 -OutputPath .\\pujjo-inventory.json
```

This produces a local JSON report only; it doesn't access raw firmware or change drivers.
