# Pujjo recovery DMIC evidence (review only)

Source: user-supplied DMIC-Evidence.zip extracted from Nissa recovery 16836.33.0. ZIP CRC check passed. Board topology: lib/firmware/intel/sof-tplg/pujjo/sof-adl-rt1019-rt5682.tplg, 55903 bytes, SHA-256 9ca138df2d471e9da3699566d2131a56cb9a0bc3274b074e7cc78e668ff5c697. This is the already pinned IPC3 source. Manifest private bytes at 148: 03 17 00. No recovery binaries are redistributed.

## Decoded source parameters

ALSA topology ABI 5 physical link block at byte 31667 (36-byte header). First entry at 31703, name dmic01, link ID 1, fixed header 1656 bytes, private size 300. Private vendor arrays at 33359, 33443, 33503, 33523, 33591. SOF token definitions from Linux UAPI sound/sof/tokens.h.

- DAI type DMIC, index 0 (155=0).
- Tokens 600=1, 601=2400000, 602=4800000, 603=40, 604=60, 608=48000, 609=32, 610=200, 605=2.
- PDM controller 0 and 1: 700=controller ID, 701=1, 702=1, 703=0, 704=0, 705=0, 706=0 (A/B enabled, normal polarity, clock edge 0, skew 0).
- PCM entry at 23459, ID 99, capture only, capture capabilities at 24255: format bitmask 0x400 (S32_LE), 48000 Hz only, 4 channels minimum and maximum. No explicit significant bits.
- Separate DMIC1 DAI index 1 dmic16k at 33659: 16 kHz, 32-bit FIFO, one active PDM controller (0), A/B enabled, 400 ms ramp. Not a DMIC0 substitute.

UCM source HiFi.conf profiles under usr/share/alsa/ucm:
- 2uf.pujjo: PCM 99 internal mic, channel map starts 0,1. SHA-256 6b79daf111b536901dbf46ab9004d72e05c555cf3d872d7a55a1f51c9c8b603e.
- 2uf1wf.pujjo: PCM 99 front mic 0,1; rear mic 2,2. SHA-256 795baf6df7de28782b87f1bfc55e61d8e9e959a1c23ae538269881cc8da43956.
- 1uf.pujjo1e: PCM 99 internal mic 0,1. SHA-256 ab90a664e6d41d5d390dd23591b15d38a63acf0c52f07b5b208959580ae24131.

Recovery DSP image sof-adl-n.ri is 725760 bytes, SHA-256 56f64335f87ff20345e20e1757dc474bc70f6f337f6f3d3bfeec6d7faa1f2ac6; distinct from the pinned IPC4 image. Not a proposed substitute.

## Porting implications

Current AudioSelectFormat in PujjoAudioBus.c requires stereo at 48 kHz. Recovery PCM99 requires four-channel S32_LE while UCM maps logical microphone devices from that stream. Do not simply change the endpoint channel count or discard a PDM controller. Review IPC4 DMIC encoder input, gateway attributes and FIR/decimation configuration, and verify either supported four-channel capture with explicit channel selection or a separately justified stereo hardware configuration. Exact active physical mics and final PDM clock are not demonstrated by these topology tokens; the min/max bounds do not select a frequency.

Status: recovery configuration decoded, not an IPC4 gateway payload, not hardware qualification. Keep fallback and activation guards unchanged. No original files or private paths committed.
