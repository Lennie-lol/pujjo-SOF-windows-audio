# Changes in v0.0.16 — 2026-11-17

- Hardened the production IPC4 metadata parser: $AM1/$AME identifiers, nonempty bounded module tables, overflow-resistant sizes and configuration ranges. Added malformed-image cases and 20,000 deterministic fuzz inputs. Parsed a real official ADL-N 2.12 image; parsing is not signature authentication.
- Removed the physical-address DMA fallback. Production allocation now requires a WDF DMA enabler and uses the common buffer's device logical address; failure leaves cleared output. Added tests against the actual allocator function.
- Added persistent device-parented wait locks and explicit powered state to both Windows Resource Hub transports. Serialized open/I/O/power/close, prohibited I/O outside D0, retained locks through resource rebalance, and cleaned failed targets. No guessed connection IDs or GPIO polarity.
- Updated the optional codec probe lifecycle to initialize its transport once, use passive callbacks, and invalidate stale resource evidence. Real probe I/O remains opt-in; GPIO remains disconnected.
- Added a pure RT5682S PLL2 coefficient planner for measured 19.2/24/48 MHz MCLK to 24.576 MHz. It returns five masked register updates, performs no I/O and does not imply PLL lock, calibration or correct board wiring.
- Fixed coordinator fault-stop handling to retry mute/disable without clearing the fault latch. Documented its caller-serialized contract. It remains a host-only model.
- Corrected the main DSP project from desktop Application/v143 to KMDF Driver/WDK toolset, KMDF 1.33 build properties, inherited kit paths/default driver libraries, and standard framework startup. Removed the manual security-cookie/FxDriverEntry wrapper.
- Split portable CI checks from the independent Debug/Release Windows jobs so a portable failure does not prevent a Windows build attempt. CI retains logs and labels any future products NOT-INSTALLABLE-NOT-HARDWARE-VERIFIED.
- Added full-source Windows transport tests using WDF doubles, including deterministic concurrent close/read, 1,000 power cycles, short transfers, failed opens, failed sends and reopen. Expanded strict, sanitizer and GCC analyzer matrix.

No hardware activation or security bypass was added. Existing licenses and upstream source remain preserved. Current results supersede inherited audit reports; historical results are not validation of this release.
