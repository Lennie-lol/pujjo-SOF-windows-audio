#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Source/project checks: not substitutes for executing Windows kernel paths."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
loader=(root/'src/driver/Loader.c').read_text()
assert 'ioStatus.Information != fileSize' in loader
# Failure is now latched under the IPC lock before examining bootState.
end=loader[loader.index('status = KeWaitForSingleObject(&Ctx->FwReadyEvent'):]
assert 'Ctx->FwState = SOF_FW_BOOT_COMPLETE' not in end
failure=end.split('if (status != STATUS_SUCCESS || Ctx->IpcFaulted',1)[1].split('KeReleaseSpinLock',1)[0]
assert 'bootState != SOF_FW_BOOT_COMPLETE' in failure
assert 'Ctx->FwState = SOF_FW_BOOT_FAILED;' in failure
assert 'Ctx->IpcFaulted = TRUE;' in failure
assert 'bootState = SOF_FW_BOOT_FAILED;' in failure
assert end.index('bootState = SOF_FW_BOOT_FAILED;') < end.index('if (bootState == SOF_FW_BOOT_COMPLETE)')
assert 'return status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : STATUS_DEVICE_NOT_READY;' in end
ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
for project in root.glob('*.vcxproj'):
    tree=ET.parse(project)
    for node in tree.findall('.//m:ClCompile',ns)+tree.findall('.//m:ClInclude',ns)+tree.findall('.//m:ProjectReference',ns):
        name=node.get('Include')
        if name: assert (root/name.replace('\\','/')).is_file(),(project,name)
    for node in tree.findall('.//m:PreprocessorDefinitions',ns):
        definitions=node.text or ''
        for flag in ['PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT','PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED',
                     'PJ5682S_ENABLE_READONLY_PROBE','PJ5682S_TEST_ONLY_WRITES',
                     'PJ1019_TEST_ONLY_CALLBACKS','PJ1019_TEST_BUILD']:
            assert flag not in definitions,(project,flag)
workflow=(root/'.github/workflows/compile-windows.yml').read_text()
assert 'run_offline_suite.py' in workflow and 'if: always()' in workflow
assert '/bl:' in workflow and '/v:normal' in workflow
assert '$LASTEXITCODE' in workflow
print('PASS: source fail-closed checks, project source references and CI logging checks')
