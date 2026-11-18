# Pujjo rev3 Windows observations (2026-11-18)

User-supplied read-only Windows PowerShell/PnPUtil output, not a test of this driver.

| Item | Observed |
| --- | --- |
| SMBIOS board | Google Pujjo rev3 |
| Firmware | coreboot MrChromebox-2609.0, 2026-09-26 |
| Audio DSP | PCI 8086:54C8, subsystem 00000000, revision 00, HDAS / PCI 1F:03; code 28 |
| Codec | ACPI RTL5682, I2C3.RT58; code 28; reported IRQ 1024 |
| Amplifier | ACPI RTL1019, I2C3.ALCP; code 28 |
| I2C3 parent | PCI 8086:54EB, subsystem 72708086; started, Intel ialpss2_i2c_adl_n.inf 30.100.2422.24, WHCP-signed per PnPUtil, IRQ 40 |
| GPIO controller | ACPI INTC1057; started, Intel ialpss2_gpio2_adl_n.inf 30.100.2422.24, WHCP-signed per PnPUtil, IRQ 14 |
| DSP reported memory spans | DEEAC000-DEEAFFFF (16 KiB); DEF00000-DEFFFFFF (1 MiB) |

All three audio nodes reported problem status 0xC0000490. The host controllers are present and started; their presence does not demonstrate working codec transactions or amplifier control. No need to replace those host drivers based on this evidence. The reported DSP memory spans do not prove BAR ordering; the interrupt and memory resources are machine-specific observations, not hardcoded driver configuration.

Remaining evidence: translated Resource Hub connections, codec identity, firmware provenance and boot, runtime interrupt and DMA ownership, speaker polarity and gain/calibration, signed/rollback-tested diagnostic package. No live DSP boot, codec writes, GPIO activation or audio playback is authorized by these observations.
