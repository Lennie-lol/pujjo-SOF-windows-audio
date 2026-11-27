# Pujjo ROM and microphone qualification

The v9.2 physical transcript recorded PCI command 0x0006, seven capture and nine playback descriptors, reset types 0 with a handler, IOMMU domain mask 3 with no domain attached, and six unpublished buffers created and destroyed. The embedded 1848-byte route selected a 2.4 MHz PDM clock. These observations leave the lifetime of an address after hardware publication unresolved.

## Next physical observation

Use only the unsigned v12 ROM-readiness submission after verifying its ZIP digest, internal manifest, revision, and the existing local review and signing procedure. Preserve the disabled driver and recovery exports. Collect one cold, one-shot read-only PrepareHardware transcript on the specified 8086:54c8 Pujjo. Save the complete log from canary consumption and persistent disarm through ReleaseHardware, together with the device instance, installed INF and package digest. Record PCI capability parser status, PCIe/FLR/pending/PM fields, reset type mask, BAR lengths, idle audit, PPCTL, foreign-stream mask and ADSPCS. Record missing fields explicitly. A missing disarm confirmation, unexpected hardware write, or mismatched device/package ends the test.

## Conditions for a later boot experiment

The updated readiness build retains the v12 diagnostic INF version. Identify it by the Actions run and ZIP SHA-256. In its audit breadcrumb, `stream descriptor` identifies the zero-based slot, `controller DMA base offset` identifies the BAR0 register offset, and `0xffffffff` means the stage has no index. No DMA address is logged. A powered primary core held in reset now fails the cold audit because its prior power owner is unknown.

Even an idle v12 result cannot authorize address publication. Establish from the platform and Windows bus/DMA contracts which component exclusively owns these descriptors, which mechanism prevents new device requests, how outstanding transactions complete, and how mappings stay valid through orderly stop, rebalance and surprise loss. The zero reset type mask is an unresolved observation. Keep the admission gate closed until those conditions are independently demonstrated.

Only after qualification, a separate ROM-only experiment must show ICCMAX preparation, PURGE acknowledgement, INIT_DONE, authenticated execution of the pinned image, FW_ENTERED after both loaders stop, IPC4 FW_READY and a matching four-field FW_CONFIG version. Record elapsed times, exact failure stage, final ownership ledger, controller restoration, DSP power-down and persistent disarm. A timeout, inaccessible register or uncertain detach ends that attempt. No automatic re-arm.

## Physical capture acceptance

After successful ROM and shutdown qualification, exercise only the embedded DMIC graph. Record module replies, stream tag, buffer geometry and packet positions. Test STOP → ACQUIRE → PAUSE → RUN, pause/resume, stop/reopen, orderly removal and later the qualified loss path. Confirm a fresh packet epoch after STOP, no advancement while paused, and correct error delivery after an interrupted stream.

Record 48 kHz mono PCM through WASAPI and Sound Recorder. Save a recording containing quiet, a newly chosen spoken phrase and quiet again. Verify the phrase by listening, check duration, clipping, gaps and timestamps, and confirm the selected endpoint by its device instance with alternate capture inputs disabled. Repeat through Discord with a remote listener identifying the phrase. Retain the evidence for each client independently.

## Implementation references

- [Linux SOF HDA ROM loader and acknowledgement order](https://github.com/torvalds/linux/blob/master/sound/soc/sof/intel/hda-loader.c)
- [Microsoft WaveRT input packet numbering and timestamps](https://learn.microsoft.com/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertinputstream-getreadpacket)
- [Microsoft device reset interface](https://learn.microsoft.com/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard)
