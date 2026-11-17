# Pujjo Windows SOF — IPC4 foundation before speaker work

**Development-only; NOT installable, NOT hardware verified, and NOT proof of working DSP/audio.**

## What this change adds

- Portable / WDK-compiled `PujjoIpc4Protocol.[ch]`: a bounds-checking
  little-endian IPC4 header+payload framing helper, matching the documented
  primary header bit fields and 4096-byte maximum payload test policy.
  Recognizes global FW_READY notifications only when the target, type,
  direction and reserved bit are valid.
- Portable / WDK-compiled `PujjoIpc4Transaction.[ch]`: a fail-closed logical
  transaction lifecycle, including reentrancy limits, timeout, stale host
  generation tokens, fault latching, and reset. **Production behavior is
  locked** without `PJ_IPC4_TRANSACTION_HOST_TEST`, which is forbidden in
  `_KERNEL_MODE`; it is not wired to the actual doorbell/interrupt system.
- Hardening of existing `SofIpcSendMessage` to reject invalid timeouts and
  missing, oversized or mailbox-truncated payloads, and to zero-pad the last
  partial DWORD rather than reading beyond the input buffer. Removed an
  unsafe debug routine that read six DWORDs when only four were available.
  FW_READY recognition also now validates an actual global notification header.
- `sof_dsp.vcxproj` compiles both protocol-only modules in Debug/Release.
  Neither sends packets nor enables firmware loading. No INF, CAT, or EXE
  installer has been added.
- Two extra offline regression suites, compiled with native C compilers on CI,
  plus static enforcement of the WDK source inclusion and runtime gates.

## Limits and unfinished non-speaker layers

This is *not* a full Windows SOF IPC4 transport implementation. The existing
legacy `Ipc.c` still needs independent live-path review for reply correlation,
interrupt ownership/ack ordering, firmware notification generations, memory
barriers, mailbox sizing, DMA limits, thread safety, exception paths and
D0/D3 cleanup. Host-only generations cannot magically distinguish old real
DSP replies because that information is **not** transmitted in IPC4 headers.
No validated on-device ROM boot or firmware compatibility test exists.
The firmware metadata parser is not signature verification. Windows audio
endpoint/PortCls integration still requires independent validation.

The *next meaningful non-speaker* step requires read-only on-device resource
mapping and authenticated firmware/topology evidence, followed by an audited
WDK backend and controlled hardware diagnostics. Do not modify the boot flag
or install `.sys` files just because this change compiles.

## How to validate

From a clean source checkout:

    python tests/test_ipc4_protocol_static.py
    python tests/run_ipc4_protocol_tests.py
    python tests/run_offline_suite.py --logs build-logs/offline

On Windows, the portable C runner requires an installed GCC/Clang toolchain.
The actual Windows WDK source compile remains available via GitHub Actions
and the separate Windows VM build script. Physical audio remains untested.
