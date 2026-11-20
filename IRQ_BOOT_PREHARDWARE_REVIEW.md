# IRQ + firmware-boot prehardware review

The ISR now checks each necessary BAR4 IPC register's mapped span before reading or masking it. For ACE, it verifies the interrupt-pointer register and checks the firmware-derived offset *before* adding the IRQ status offset, preventing out-of-bounds or wraparound reads.

The firmware loader now refuses to automatically retry unreviewed, hardware-specific "community" firmware after a ROM/DMA timeout. It marks a boot attempt in progress before triggering DMA; the IPC DPC only treats a correctly framed FW_READY message as success during that boot phase. **FW_READY framing is not cryptographic authentication or physical proof of compatibility.**

These are source-level guards under a currently disabled firmware-boot path. The physical IRQ hierarchy, interrupt acknowledgement ordering, HDA/DMA engine behavior, firmware authenticity, real concurrency, teardown, power-state handling, and endpoint routing still require independent verification. No installer, firmware image, codec/amp activation, or speaker output is supplied.

## Exact upstream firmware candidate regression

The offline CI now obtains the Intel-signed ADL-N IPC4 v2.12 candidate from
sof-bin commit `fcd5ae85ead7ecd67ff199335b61fac825843ca0`, verifies Git blob
identity, length and SHA-256 against the production loader's pin, then executes
the production manifest catalog and loader parser against the actual file.
The catalog test checks the two module UUIDs and the 48 kHz stereo gateway
budgets using the production low-latency planner. Only metadata reports enter
CI artifacts; the firmware image is not staged or executed. A passing result
does not establish ROM authentication, runtime IPC compatibility or physical
boot. All activation gates remain unchanged.
