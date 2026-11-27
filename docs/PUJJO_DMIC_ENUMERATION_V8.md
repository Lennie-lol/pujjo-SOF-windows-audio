# v0.0.8.0 — microphone enumeration integration (development, not activation)

**No working Windows microphone is claimed.** The physical v0.0.6.0 resource
probe is the last successful hardware result. Windows on the board did not
provide a native ACPI NHLT and no input endpoint was present. Firmware has
never been successfully cold-booted on this Windows installation.

## What changed

The isolated 1,955-byte, one-endpoint DMIC NHLT generated from the pinned ALSA
encoder in v7 is now checked-in as C source. The generator pins SHA-256
`0f6b13f2b1440e1624d53e45373093ede80ee662d97a998e817bdfec72c5d3df`.
The portable CI job independently regenerates the bytes and compares the
entire header. The production NHLT parser independently validates the
endpoint count, payload, checksums and four raw capture slots at 48 kHz,
32-bit (768 bytes/ms). No SSP or SoundWire configuration is embedded.

The new **compile-only** `PUJJO_DMIC_ENUMERATION_CANDIDATE` configuration
connects this table to actual route selection only when the native firmware
table is absent (STATUS_NOT_FOUND or STATUS_OBJECT_NAME_NOT_FOUND). A present
but malformed or inaccessible native table is never replaced. It validates
the admitted profile before boot; the existing PortCls child's format
selection and IPC4 graph construction check the exact profile again. The
candidate excludes the analog feature flag, other NHLT fallback and SDCA
filter classification. It does not simulate a Windows microphone device.
Its DriverEntry refuses execution, and its firmware loader independently
refuses boot. CI builds it separately in both WDK configurations and cannot
stage it as the diagnostic parent. The separately staged DMIC PortCls INF
is still review-only.

The loader's code-load and ICCMAX HDA descriptors now use a common stop
and detach proof: bounded RUN clear; reject unavailable status; clear
control and verify RUN/SRST; clear both BDL address halves and verify;
then read control again. Transfer errors remain separate from proof of
descriptor detachment. Fault-injected host MMIO tests include lost writes,
stuck RUN, late transfer errors and inaccessible status or final control.
This improves normal and error-stop checks. It does **not** establish
surprise-remove/failed-start lifetime guarantees for the WDF common buffers.

## v8 diagnostic and next hardware gate

The separate 0.0.8.0 read-only resource diagnostic retains the durable
single-shot disarm, exact board/PCI protection and no-DMA/no-IRQ/no-D0
activation path. After unmapping BARs, it reports the native table,
validates the embedded candidate as evidence only, and queries the bus
`GUID_DEVICE_RESET_INTERFACE_STANDARD` interface. It reports status or
the advertised supported reset types and whether a handler exists,
releases the interface reference, and never invokes a reset.

A function-level reset, if actually supported, may help establish a
recovery path for a malfunctioning PCI function. A platform-level reset
may affect other devices and is outside the microphone test scope.
The mere availability of a reset interface is not proof that a reset
works or that DMA mappings survive framework teardown. See Microsoft's
[reset interface contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard)
and [framework object lifecycle](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/framework-object-life-cycle).

The diagnostic is unsigned CI output, not the active microphone driver.
Retain the known-good signed v6 and v2 fallback, WinRE and recovery media.
The existing local signing checks are required before any proposed
physical read-only diagnostic. Do not install the compile-only candidate,
stage the PortCls review child, invoke resets or arm a firmware boot.

To achieve a real Windows input endpoint: establish removal-safe DMA
ownership/quiescence (boot and runtime capture), pass physical pinned
firmware ROM entry, FW_READY and FW_CONFIG, qualify the DMIC profile with
the running DSP, and bind a separately verified PortCls child. Then verify
the Windows recording endpoint and real samples. None of those physical
checks can be established by a host compiler or a synthetic endpoint.
