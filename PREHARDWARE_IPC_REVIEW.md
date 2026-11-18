# Pre-hardware IPC review

Development-only. Default firmware boot guard unchanged. No installer, physical speaker, codec, or amplifier changes.

This iteration closes a caller-capacity overwrite in the legacy IPC reply path by restricting the currently used response interface to headers only. The sender refuses payload-bearing output requests before writing registers. No generic IPC4 reply length can be inferred from mailbox capacity. Future payload commands need a command-specific length contract.

Receive ownership remains with the submitting thread through consumption; reply type and target must match, and timeout/firmware exception latches a fault instead of allowing reuse of the untagged IPC session. This is not a proof of wire correlation: same-type late or duplicate messages require real reset/interrupt validation. Register span checks use subtraction to avoid wraparound. The sender no longer calls its DPC concurrently while waiting. Legacy automatic exception dump and streaming log drain are deferred; file breadcrumbs require valid passive execution context.

Remaining physical blockers: authenticated firmware/topology compatibility, translated resources, actual ROM/IPC readiness, interrupt ownership/ACK semantics, DMA/IOMMU, teardown races and reset recovery. Real DSP boot, signed packaging, endpoints, codec and amplifier activation are still unavailable. The inherited firmware loader is not validated on physical hardware. Static contracts and host tests do not certify driver runtime safety.
