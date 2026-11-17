# Pujjo SOF v0.0.17 — GitHub Actions Run #5 Hotfix

Failure: MSB4057, `GetDriverProjectAttributes` missing in sof_core.vcxproj.

`sof_dsp.vcxproj` is a KMDF driver and references `sof_core.vcxproj`,
a normal v143 static library. The WDK driver target queries its referenced
project for driver metadata even though the static library has none.

Fix: Add a no-op `GetDriverProjectAttributes` target to sof_core.vcxproj,
returning an empty `@(DriverProjectAttributes)` list, while preserving v143.
Add a regression assertion to tests/test_windows_ci_v017.py.

Usage: extract the ZIP into the ROOT of your current GitHub repository,
keeping subfolder paths. Replace ONLY the two source files named below.
`HOTFIX_NOTES.md` is an optional informational file. Commit and push,
then rerun GitHub Actions.

Local tests: 26/26 offline suite commands passed (and the targeted test
was rerun afterward). The revised actual Windows WDK Debug/Release builds
are NOT verified. Hardware activation remains disabled.

Files changed:
 - sof_core.vcxproj
 - tests/test_windows_ci_v017.py

No C/C++ source, signing mode, activation gates or version changed.
