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
assert end.count('Ctx->FwState = SOF_FW_BOOT_COMPLETE') == 1
assert end.index('status = SofQueryBootedFirmwareVersion') < end.index(
    'if (!Ctx->IpcFaulted && Ctx->FwState == SOF_FW_BOOT_READY_RECEIVED)') < end.index(
    'Ctx->FwState = SOF_FW_BOOT_COMPLETE')
ipc=(root/'src/driver/Ipc.c').read_text()
send = ipc.split('NTSTATUS\nSofIpcSendMessage(', 1)[1].split('\n}', 1)[0]
assert 'SofIpcSendWhilePowered(Ctx, Msg, Reply, TimeoutMs, SOF_FW_BOOT_COMPLETE)' in send
query = ipc.split('NTSTATUS SofIpcGetBootFirmwareVersion(', 1)[1].split('\n}', 1)[0]
assert 'SOF_FW_BOOT_READY_RECEIVED' in query and 'SOF_IPC4_FW_PARAM_FW_CONFIG' in query
failure=end.split('if (status != STATUS_SUCCESS || Ctx->IpcFaulted',1)[1].split('KeReleaseSpinLock',1)[0]
assert 'bootState != SOF_FW_BOOT_READY_RECEIVED' in failure
assert 'Ctx->FwState = SOF_FW_BOOT_FAILED;' in failure
assert 'Ctx->IpcFaulted = TRUE;' in failure
assert 'bootState = SOF_FW_BOOT_FAILED;' in failure
assert end.index('bootState = SOF_FW_BOOT_FAILED;') < end.index('if (bootState == SOF_FW_BOOT_READY_RECEIVED)')
assert 'status = status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT :' in end
assert 'status == STATUS_SUCCESS ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_DEVICE_NOT_READY' in end
assert end.index('status = status == STATUS_TIMEOUT ?') < end.index('goto boot_failed;', end.index('status = status == STATUS_TIMEOUT ?'))
ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
for project in root.glob('*.vcxproj'):
    tree=ET.parse(project)
    for node in tree.findall('.//m:ClCompile',ns)+tree.findall('.//m:ClInclude',ns)+tree.findall('.//m:ProjectReference',ns):
        name=node.get('Include')
        if name: assert (root/name.replace('\\','/')).is_file(),(project,name)
    for node in tree.findall('.//m:PreprocessorDefinitions',ns):
        definitions=node.text or ''
        for flag in ['PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT','PUJJO_EXPLICIT_IPC4_FIRMWARE_REVIEWED',
                     'PUJJO_ENABLE_EXPERIMENTAL_AUDIO','PUJJO_ENABLE_EXPERIMENTAL_ANALOG',
                     'PJ5682S_ENABLE_CONTROL','PJ1019_ENABLE_CONTROL',
                     'PJ5682S_ENABLE_READONLY_PROBE','PJ5682S_TEST_ONLY_WRITES',
                     'PJ1019_TEST_ONLY_CALLBACKS','PJ1019_TEST_BUILD']:
            assert flag not in definitions,(project,flag)
workflow=(root/'.github/workflows/compile-windows.yml').read_text()
assert 'run_offline_suite.py' in workflow and 'if: always()' in workflow
assert '/bl:' in workflow and '/v:normal' in workflow
assert '$LASTEXITCODE' in workflow
print('PASS: source fail-closed checks, project source references and CI logging checks')
