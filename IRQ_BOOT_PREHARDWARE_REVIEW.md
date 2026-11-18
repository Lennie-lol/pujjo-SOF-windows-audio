# IRQ + firmware-boot prehardware review

The ISR now checks each necessary BAR4 IPC register's mapped span before reading or masking it. For ACE, it verifies the interrupt-pointer register and checks the firmware-derived offset *before* adding the IRQ status offset, preventing out-of-bounds or wraparound reads.

The firmware loader now refuses to automatically retry unreviewed, hardware-specific "community" firmware after a ROM/DMA timeout. It marks a boot attempt in progress before triggering DMA; the IPC DPC only treats a correctly framed FW_READY message as success during that boot phase. **FW_READY framing is not cryptographic authentication or physical proof of compatibility.**

These are source-level guards under a currently disabled firmware-boot path. The physical IRQ hierarchy, interrupt acknowledgement ordering, HDA/DMA engine behavior, firmware authenticity, real concurrency, teardown, power-state handling, and endpoint routing still require independent verification. No installer, firmware image, codec/amp activation, or speaker output is supplied.
