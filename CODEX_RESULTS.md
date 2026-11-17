# Development report — Pujjo SOF v0.0.16

This release implements source and test improvements but does not deliver working Windows audio. No Windows driver binary was produced or installed. Unverified DSP startup and amplifier activation remain disabled by default.

CHANGES.md lists the implementation, TEST_RESULTS.md records executed checks, RESEARCH_v0.0.16.md records references, and REMAINING_BLOCKERS.md gives the prerequisites for hardware validation. Source inventory and an input-to-output change list accompany the logs.

## Firmware and DMA

The actual official Intel-signed ADL-N IPC4 2.12 image was retrieved and tested, rather than assuming the .ri extension implied an ELF or legacy SOF image. It has a $AE1 extended manifest and $AM1 binary header at payload offset 0x2000; 25 $AME modules were parsed. The new bounds checks reject malformed module counts/configuration ranges before walking them. This remains a metadata parser: extended-manifest semantic validation, executable segment validation, platform/authentication policy and ROM acceptance are incomplete. No signature check was bypassed; downloaded firmware is not redistributed here.

The production DMA allocator now requires WDF common-buffer allocation and its logical device address. It no longer substitutes a CPU physical address for an IOMMU device address. DMA engine programming, interrupt completion and quiescence still require implementation/review and real validation.

## Peripheral and power lifecycle

Both Resource Hub transports now synchronize their live targets with device-parented wait locks, perform synchronous transfers only in the explicit powered state and serialize close with outstanding transfers. Open failures delete temporary targets; close clears powered/open state without destroying the lock needed for later PrepareHardware. Codec probe callbacks retain the transport object across rebalance and enforce passive execution. These source changes were exercised with fake WDF APIs and concurrent pthread calls; real WDF behavior remains unverified.

The amplifier GPIO API expresses physical voltage level, not guessed active polarity. It is still not called by the PnP probe. The RT1019P model's calibration and shutdown tests remain simulated. The RT5682S planner returns masked PLL2 coefficient values for three measured input clocks, but does not execute register writes or complete codec initialization. This boundary prevents an unverified clock choice from activating audio hardware.

## Coordination and audio endpoints

The coordinator previously did nothing on stop once fault-latched, even if the first mute/disable callbacks failed. It now retries both shutdown requests while retaining the fault latch. Its API explicitly requires caller serialization. It remains disconnected from the real Windows driver because its mock firmware/audio callbacks are not valid kernel backends. No new WaveRT render/capture endpoint or validated Windows topology was completed. Existing PortCls/IPC scaffolding must not be mistaken for a functional pipeline.

## Windows build status

Both requested msbuild commands were attempted and failed before compilation because MSBuild is absent. There is no Windows VM or WDK in this environment. The available GitHub account could not access the candidate project repository (404); the local gh client had no authenticated session and no usable workflow-dispatch interface was available. No repository was created, pushed or workflow launched.

The main project incorrectly used a desktop application toolset with hand-selected kernel paths/libraries. It now uses WDK KMDF build configuration and standard startup. KMDF 1.33 is the documented Windows 11 baseline selected through MSBuild properties. The separate Windows CI jobs retain Debug/Release logs and future intermediate products, and portable tests run independently. All build configuration changes remain unverified by MSVC; this is not a successful Windows compilation.

## Readiness and limitations

Suitable for further offline development and compile-only CI, not for installing or enabling audio on a Chromebook. Successful portable tests cover selected algorithms and lifecycle regressions, not the entire Linux-derived tree or Windows runtime. No claim of zero bugs, complete memory safety, verified gain limits, firmware trust or electrical safety is made. The licenses and existing source tree are retained; no proprietary activation system was introduced or bypassed.
