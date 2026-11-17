#!/usr/bin/env python3
"""Read-only audit of Linux SOF topology evidence for the Pujjo port.

Usage: python tests/verify_linux_topology.py chromebook-linux-audio-main.zip
No Windows driver installation, device writes or firmware loading.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
HDR = ROOT / "src/driver/PujjoLinuxTopology.h"

def verify_header():
    h = HDR.read_text()
    expected = {
        "#define PUJJO_LINUX_SSP_SPEAKERS          1U",
        "#define PUJJO_LINUX_SSP_HEADSET           0U",
        "#define PUJJO_LINUX_SSP_BT_LINK           2U",
        "#define PUJJO_WINDOWS_SPEAKER_PATH_READY  0U",
        '#define PUJJO_AUDIO_ACPI_CODEC            "RTL5682"',
        '#define PUJJO_AUDIO_ACPI_AMPLIFIER        "RTL1019"',
    }
    for line in expected:
        assert line in h, f"Reference mapping mismatch: {line}"
    print("PASS: Windows fork retains the documented reference mapping and fail-closed status")

def check_linux_archive(path):
    with zipfile.ZipFile(path) as z:
        bash_name = next(n for n in z.namelist() if n.endswith("/setup-audio"))
        tplg_name = next(n for n in z.namelist() if n.endswith("/blobs/adl/sof-adl-rt1019-rt5682.tplg"))
        script = z.read(bash_name).decode("utf-8")
        blob = z.read(tplg_name)
    for evidence in [
        '["RTL5682"]="rt5682s"',
        '["RTL1019"]="rt1019p"',
        '"google_nissa"',
        'install_downstream_tplg "blobs/adl/sof-adl-rt1019-rt5682.tplg"',
    ]:
        assert evidence in script, f"Missing setup-audio mapping: {evidence}"
    for token in [
        b"PCM0P", b"Speakers 0", b"PIPELINE.1.SSP1.OUT", b"SSP1-Codec",
        b"PCM1P", b"PCM1C", b"Headset 1", b"SSP0-Codec",
        b"PCM7P", b"PCM7C", b"Bluetooth 7", b"SSP2-BT",
    ]:
        assert token in blob, f"Missing topology label: {token!r}"
    print(f"PASS: Linux script and {len(blob):,}-byte topology contain expected evidence")
    print("Reference routing: speakers=SSP1, headset=SSP0, digital Bluetooth link=SSP2")
    print("WARNING: This is topology inspection, not Pujjo hardware verification or Windows playback.")

if __name__ == "__main__":
    verify_header()
    if len(sys.argv) > 1:
        check_linux_archive(Path(sys.argv[1]))
    else:
        print("Linux ZIP not supplied; only local reference mapping checked.")
