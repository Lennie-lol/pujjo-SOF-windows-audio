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
encoder outputs. The same table then passes through the production analog profile verifier
for headphone, headset microphone and speaker; wrong port, altered
bytes, wrong PCM geometry and damaged tables must fail. This is a software
integration test only, not authorization to include the candidate in the
driver or activate analog hardware.

Zephyr v4.2.0 cAVS clock definitions corroborate the 38.4 MHz XTAL
(selector 0) and 24.576 MHz cardinal source (selector 1). The proposal
retains the encoder's ACS bit pending precise cAVS reserved-field review;
the actual firmware's M/N state and clock behavior are unproven.

Remaining: inspect the first real encoder CI result,
verify source selectors against the running ADL-N firmware, M/N state and
reserved fields, then qualify on-device timing and peripherals. The output
is not automatically consumed by the Windows build. The empty approved
analog profile table and all activation guards remain unchanged.

## Pujjo rev3 offline candidate audit

User-reported Windows inventory identifies Google Pujjo rev3 with
MrChromebox-2609.0, build 26300, DSP 8086:54c8 and the RTL5682 and RTL1019
nodes under the working 8086:54eb I2C controller. The Windows NHLT query
returned 1168 (not found). These observations are board identity evidence,
not an electrical qualification. CI now independently audits the exact
three SSP endpoint records against the fixed candidate and regenerates the
complete table from the pinned ALSA encoder outputs. The JSON audit makes
missing DMIC0 explicit. No DMIC gateway bytes have been fabricated and no
reviewed kernel header or analog approval has been emitted.

## Independent clock and peripheral review

Zephyr v4.2.0 drivers/dai/intel/ssp/ssp_regs_v2.h defines MCDSS at
bits 17:16, MNDSS at 21:20, SCR at 19:8, ECS at bit 6 and ACS at
bit 30. intel_adsp_cavs25.dtsi identifies the 38.4 MHz and
24.576 MHz clocks; ssp.c uses divider minus two for non-bypass MCLK,
and the IPC4 v1.5 clock control accepts one divider. The candidate
audit now calculates both expected 48 kHz stereo wire clocks from
the encoded selectors, dividers and pad fields, and rejects mutated
clock geometry. The full-register clock control is shared, so the
existing single analog lease remains essential. Unity M/N is an
assumption, and ACS is retained pending running firmware qualification;
the reference contains both a FIXME and an ACS assignment.

Linux v6.13 sound/soc/codecs/rt5682s.c supports direct PLLB coefficients
M=2 N=6 K=3 for 19.2 MHz to 24.576 MHz, corroborating the existing
planner. Its cold probe also performs supply setup, a minimum 50 ms
initial wait, identity verification, reset, patching, a 20 ms wait,
headphone calibration with bounded polling, and bias/charge-pump setup.
The Windows clocked control path is NOT an equivalent cold initialization:
it depends on a prequalified analog path and verified identity, then
journals masked clock/routing changes with reverse-order rollback.
No qualified board power/calibration program exists yet. A PLL delay
is not proof of lock; actual padded 25-bit slot and depop behavior
remain unverified. Do not enable a generic reset/patch sequence.

Linux v6.13 sound/soc/codecs/rt1015p.c matches RTL1019 and uses a
logical SDB GPIO, first-enable 300 ms delay, disable after playback,
and recalibration after suspend. Windows uses translated Resource Hub
IDs with explicit polarity, requires a muted DSP and qualified gain
limit, and attempts shutdown on failure. The delay corroborates the
lower bound but does not prove physical calibration or polarity.
Actual GPIO assignment, safe output level and power sequencing must
be measured. No amplifier activation or analog profile approval changed.

References: https://github.com/zephyrproject-rtos/zephyr/tree/v4.2.0/drivers/dai/intel/ssp
https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt5682s.c
https://github.com/torvalds/linux/blob/v6.13/sound/soc/codecs/rt1015p.c
