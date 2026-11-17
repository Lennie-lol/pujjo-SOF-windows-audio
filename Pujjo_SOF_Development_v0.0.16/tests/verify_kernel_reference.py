#!/usr/bin/env python3
"""Read-only check that uploaded Linux kernel ZIP includes Pujjo-relevant code.

No hardware access; no kernel code is included in the Windows fork.
"""
import re
import sys
import zipfile
from pathlib import Path

REQUIRED = {
 "sound/soc/sof/intel/pci-tgl.c": (
  (r"HDA_ADL_N", "Intel Alder Lake-N SOF PCI mapping"),
 ),
 "sound/soc/codecs/rt1015p.c": (
  (r'"RTL1019"', "RTL1019 ACPI ID handled by rt1015p"),
  (r"gpiod_set_value_cansleep", "Amplifier SDB GPIO sequencing"),
 ),
 "sound/soc/codecs/rt5682s.c": (
  (r"RT5682S_PLL", "RT5682S clock/PLL handling"),
  (r"regmap", "RT5682S register control"),
 ),
 "sound/soc/intel/common/soc-acpi-intel-adl-match.c": (
  (r"adl_rt1019p_amp", "ADL RTL1019P machine match"),
  (r"adl_rt5682_rt5682s_hp", "ADL RT5682S machine match"),
 ),
 "sound/soc/intel/boards/sof_rt5682.c": (
  (r"CODEC_RT1019P", "RT1019P link setup"),
  (r"CODEC_RT5682S", "RT5682S link support"),
 ),
}

def main(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        roots = [n[:-len("sound/soc/codecs/rt1015p.c")]
                 for n in names if n.endswith("sound/soc/codecs/rt1015p.c")]
        if len(roots) != 1:
            raise RuntimeError("Unable to uniquely locate Linux source root")
        root = roots[0]
        total = 0
        for rel, checks in REQUIRED.items():
            data = z.read(root + rel).decode("utf-8", errors="replace")
            for pattern, explanation in checks:
                if re.search(pattern,data) is None:
                    raise AssertionError(f"MISSING: {explanation}: {rel}")
                print(f"PASS: {explanation} [{rel}]")
                total += 1
        mk = z.read(root+"Makefile").decode("utf-8",errors="replace")
        vers = re.search(r"^VERSION\s*=\s*(\d+)",mk,re.M)
        if vers:
            print("Kernel major version:", vers.group(1))
        print(f"OK: {total} Linux-source reference checks; no files changed")

if __name__ == "__main__":
    if len(sys.argv)!=2:
        print("Usage: python tests/verify_kernel_reference.py <linux-master.zip>",file=sys.stderr)
        sys.exit(2)
    main(Path(sys.argv[1]))
