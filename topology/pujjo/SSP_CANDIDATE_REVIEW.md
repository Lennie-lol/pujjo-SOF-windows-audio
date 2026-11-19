# Pujjo IPC4 SSP candidate preparation

Reference encoder: [alsa-utils v1.2.13](https://github.com/alsa-project/alsa-utils/tree/v1.2.13/topology/nhlt/intel).
This is a review-only tool; it does not generate or load a driver header.

The stock ALSA SSP encoder writes slot width into the WAVEFORMATEXTENSIBLE
container and valid-bit fields, uses a zero subtype GUID, and identifies
generic SSP endpoints with device type zero. These are not the driver's
Windows PCM schema: 48 kHz stereo, 32-bit container, 24 valid bits, PCM GUID,
and NHLT analog device type 4. The tool converts that metadata without
rewriting the gateway payload. It assembles exactly SSP0 render/capture
and SSP1 render records, with an ACPI checksum.

Supply separately generated digest-pinned NHLT reference tables from the
encoder using 38.4 MHz IO for SSP0 and 24.576 MHz IO for SSP1. The latter
must use a surrogate integral MCLK (for example 24.576 MHz) because that
encoder uses one IO clock for both divisors; 19.2 MHz MCLK is not divisible
from 24.576 MHz. Supply the two separately generated bare 96-byte proposed
IPC4 v1.5 blobs with their SHA-256 digests. Compare every field: only MCDSS,
MNDSS and the lower 12 bits of MDIVR may differ. Validate expected BCLK
divisors, sample width, frame width, slot masks and proposed MCLK divider.
Record every accepted clock difference without treating it as approval.
Use --help for the exact CLI options. No generated binary is distributed.

Remaining: run against real encoder output and the actual candidate files,
verify source selectors against the running ADL-N firmware, M/N state and
reserved fields, then qualify on-device timing and peripherals. The output
is not automatically consumed by the Windows build. The empty approved
analog profile table and all activation guards remain unchanged.
