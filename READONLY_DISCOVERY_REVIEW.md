# Read-only discovery before physical hardware tests

The inherited PnP PrepareHardware path used to enable PCI memory/bus-mastering bits using SetBusData, enable HDA post-processing GPROCEN/PIE as a side effect of walking capabilities, and toggle HDA GCTL reset to recover an Immediate Command Busy bit. Those are unsafe defaults for resource *discovery* on a not-yet-qualified board.

This update removes all three writes from the discovery flow, checks the codec status MMIO offsets before reading, and logs the observed state. Any real activation must be separately reviewed, bounded and authorized after Pujjo-specific evidence is collected.

**Not proof of safe driver installation.** Windows interrupt ownership, firmware authenticity, DMA enablement and reset recovery, and any hardware operations outside the discovery function are not verified. Compile-time DSP boot and audio activation safeguards remain disabled. No installer provided.
