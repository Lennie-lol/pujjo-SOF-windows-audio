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

The offline GitHub Actions job now fetches the exact ALSA revision
`f04b9e0f1285fe25d4146906c5a8511741b8abad` and compiles its SSP encoder
and endpoint serializer without changing either. The test harness calls
the upstream public functions with the recovery-derived settings above.
The fixed candidate generator independently encodes the two proposed
register sets. The comparison rejects any difference outside the explicitly
allowed source selectors and MCLK divider. Artifacts under
`build-logs/ssp-reference` are review-only; they include both upstream
reference tables, both proposed blobs and the comparison report. This is
not a comparison against the original IPC3 bytes, which have another ABI.
The same CI step now feeds the generated candidate table and both payloads
into the host-compiled production NHLT parser. It verifies exact lookup for
SSP0 render/capture and SSP1 render, rejects unsupported formats/routes,
and cross-checks the files against the independent generator and original
encoder outputs. This is a software integration test only, not authorization
to include the candidate in the driver or activate analog hardware.

Zephyr v4.2.0 cAVS clock definitions corroborate the 38.4 MHz XTAL
(selector 0) and 24.576 MHz cardinal source (selector 1). The proposal
retains the encoder's ACS bit pending precise cAVS reserved-field review;
the actual firmware's M/N state and clock behavior are unproven.

Remaining: inspect the first real encoder CI result,
verify source selectors against the running ADL-N firmware, M/N state and
reserved fields, then qualify on-device timing and peripherals. The output
is not automatically consumed by the Windows build. The empty approved
analog profile table and all activation guards remain unchanged.
