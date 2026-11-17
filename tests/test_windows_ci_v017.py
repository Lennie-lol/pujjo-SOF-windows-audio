#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Static regression contracts for the failures diagnosed in CI runs #1/#2.

This is not an MSVC/WDK build or hardware validation.
"""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
projects = ('sof_dsp.vcxproj', 'pujjo_rt1019_probe.vcxproj',
            'pujjo_rt5682_probe.vcxproj')
for name in projects:
    tree = ET.parse(root / name)
    configs = tree.findall('.//m:PlatformToolset', ns)
    assert len(configs) == 2 and all(x.text == 'WindowsKernelModeDriver10.0'
                                      for x in configs), name
    compiles = tree.findall('.//m:ClCompile', ns)
    for compile_item in compiles:
        flags = compile_item.find('m:AdditionalOptions', ns)
        if flags is None:
            continue  # File listing, not compiler settings.
        assert '/kernel' in (flags.text or ''), (name, 'missing /kernel')
        defines = compile_item.find('m:PreprocessorDefinitions', ns)
        assert defines is not None and '_KERNEL_MODE' not in (defines.text or ''), name
        exclusions = compile_item.find('m:DisableSpecificWarnings', ns)
        assert exclusions is None or '4117' not in (exclusions.text or ''), name

for f in ('src/driver/peripheral/PujjoRt1019pGpioWindows.c',
          'src/driver/peripheral/PujjoRt5682sSpbWindows.c'):
    text = (root / f).read_text()
    assert '#include <reshub.h>' in text
    assert 'RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, idLow, idHigh)' in text
    assert 'DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE)' in text
    # Reshub.h exposes a FORCEINLINE FUNCTION, not a preprocessor symbol.
    assert 'defined(RESOURCE_HUB_CREATE_PATH_FROM_ID)' not in text
    assert 'RESOURCE_HUB_PATH_CHARS' not in text

for f in ('src/driver/experimental/PujjoRt5682sCodec.h',
          'src/driver/experimental/PujjoHeadsetClock.h'):
    text = (root / f).read_text()
    assert 'PujjoFixedTypes.h' in text
    assert '#include <stdint.h>' not in text and '#include <stddef.h>' not in text

for f in ('src/driver/experimental/PujjoRt5682sCodec.c',
          'src/driver/experimental/PujjoHeadsetClock.c'):
    text = (root / f).read_text()
    assert '#ifndef _KERNEL_MODE\n#include <string.h>\n#endif' in text
    assert 'RtlZeroMemory(' in text

fixed = (root / 'src/driver/PujjoFixedTypes.h').read_text()
assert 'typedef USHORT PJ_WORD;' in fixed
assert 'typedef uint16_t PJ_WORD;' in fixed
codec = (root / 'src/driver/experimental/PujjoRt5682sCodec.c').read_text()
assert '#error PJ5682S_TEST_ONLY_WRITES is forbidden in a Windows kernel build' in codec
assert not list(root.rglob('*.inf')) and not list(root.rglob('*.cat'))
print('PASS: CI #1/#2 regression contracts, correct WDK /kernel and Resource Hub usage')
