# IRQ + firmware-boot prehardware review

The ISR now checks each necessary BAR4 IPC register's mapped span before reading or masking it. For ACE, it verifies the interrupt-pointer register and checks the firmware-derived offset *before* adding the IRQ status offset, preventing out-of-bounds or wraparound reads.

The firmware loader refuses to retry with another image after a ROM/DMA timeout. It checks for a pending inbound doorbell immediately before triggering DMA and admits the IPC DPC after playback RUN reads back set. ROM FW_ENTERED, the cold FW_READY notification and the matching FW_CONFIG reply are separate boot requirements.

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

The runtime FW_CONFIG query now also checks the advertised DL/UL mailbox
capacities and maximum simultaneous pipelines, rather than accepting a
version tuple alone. The current transport needs 4096 bytes in each direction
and the existing DMIC plus single analog lease may need two pipelines.
Missing, malformed, duplicate or insufficient capability tuples block the
transition to BOOT_COMPLETE. This is a conservative admission requirement;
actual firmware replies have not been captured from Pujjo hardware yet.
