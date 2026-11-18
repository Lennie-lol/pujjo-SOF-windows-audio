#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Ensure pure IPC4 code builds into WDK but remains activation-locked.
Not a substitute for a physical Pujjo test.
"""
from pathlib import Path
import xml.etree.ElementTree as ET
root = Path(__file__).resolve().parents[1]
proto = (root/'src/driver/PujjoIpc4Protocol.c').read_text()
trans = (root/'src/driver/PujjoIpc4Transaction.c').read_text()
header = (root/'src/driver/PujjoIpc4Protocol.h').read_text()
assert 'PJ_IPC4_MAX_MAILBOX_PAYLOAD 4096u' in header
assert 'PJ_IPC4_NOTIFY_FW_READY 8u' in header
assert 'PJ_IPC4_DIRECTION_MASK 0x20000000u' in header
assert 'PujjoFixedTypes.h' in header
assert 'forbidden in WDK kernels' in trans
assert '#ifndef PJ_IPC4_TRANSACTION_HOST_TEST' in trans
for source in (proto, trans):
    for banned in ('WdfIoTarget', 'READ_REGISTER_', 'WRITE_REGISTER_',
                   'MmMapIoSpace', 'IOCTL_GPIO', 'IOCTL_SPB',
                   'SofStartFirmwareBoot(', 'PJ1019_TEST_ONLY_CALLBACKS'):
        assert banned not in source, (banned, source[:50])
ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
project=ET.parse(root/'sof_dsp.vcxproj')
source_names={n.attrib['Include'].replace('\\','/') for n in project.findall('.//m:ClCompile',ns) if 'Include' in n.attrib}
header_names={n.attrib['Include'].replace('\\','/') for n in project.findall('.//m:ClInclude',ns) if 'Include' in n.attrib}
for name in ('PujjoIpc4Protocol.c','PujjoIpc4Transaction.c'):
    assert 'src/driver/'+name in source_names, name
for name in ('PujjoIpc4Protocol.h','PujjoIpc4Transaction.h'):
    assert 'src/driver/'+name in header_names, name
for node in project.findall('.//m:PreprocessorDefinitions',ns):
    defines=node.text or ''
    assert 'PJ_IPC4_TRANSACTION_HOST_TEST' not in defines
    assert 'PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT' not in defines
loader=(root/'src/driver/Loader.c').read_text()
device=(root/'src/driver/Device.c').read_text()
assert '#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)' in loader
assert '#if !defined(PUJJO_ENABLE_EXPERIMENTAL_DSP_BOOT)' in device
print('PASS: IPC4 pure parsers linked into WDK; hardware boot and mocks remain locked')
ipc=(root/'src/driver/Ipc.c').read_text()
assert '#include "PujjoIpc4Protocol.h"' in ipc
assert 'pj_ipc4_is_fw_ready(primary)' in ipc
assert 'pj_ipc4_is_notification(primary)' in ipc
assert 'Msg->PayloadSize > Ctx->HwDesc->MailboxDownlinkSize' in ipc
assert '((Msg->PayloadSize + 3u) & ~3u) > Ctx->HwDesc->MailboxDownlinkSize' in ipc
assert 'WRITE_REGISTER_ULONG((PULONG)(mailbox + offset), word)' in ipc
assert 'p[5]' not in ipc and 'rb[5]' not in ipc
print('PASS: existing IPC sender validates sizes and no longer reads extra log words')

# The Windows topology entry points must use the same encoders exercised by
# portable tests, and propagate transport errors rather than pretend success.
topo=(root/'src/driver/Topo.c').read_text()
helper=topo.split('static NTSTATUS SofIpc4SendGraphPacket(',1)[1].split('#endif',1)[0]
assert 'if (!Ctx) return STATUS_INVALID_PARAMETER;' in helper
assert helper.index('Result != PJ_IPC4_VALID') < helper.index('msg.Primary = Packet->primary')
assert 'return SofIpcSendMessage(Ctx, &msg, NULL, 500);' in helper
for function,encoder in (
    ('SofIpc4CreatePipeline','pj_ipc4_pipeline_create('),
    ('SofIpc4DeletePipeline','pj_ipc4_pipeline_delete('),
    ('SofIpc4SetPipelineState','pj_ipc4_pipeline_state('),
    ('SofIpc4InitModuleInstance','pj_ipc4_module_init('),
    ('SofIpc4BindModules','pj_ipc4_module_link(1u,'),
    ('SofIpc4UnbindModules','pj_ipc4_module_link(0u,'),
):
    body=topo.split('NTSTATUS '+function+'(',1)[1].split('#else',1)[0]
    assert '#ifdef _KERNEL_MODE' in body
    assert body.index(encoder) < body.index('SofIpc4SendGraphPacket(Ctx, result, &packet);')
    if function == 'SofIpc4InitModuleInstance':
        assert body.index('pj_firmware_plan_ll(') < body.index(encoder)
        assert body.index('SofIpc4SendGraphPacket(') < body.rindex('ExFreePoolWithTag(storage, SOF_DEVICE_TAG);') < body.index('return status;')
    assert 'return STATUS_SUCCESS' not in body
print('PASS: WDK graph commands use tested encoders and propagate real transport status')

# All graph constructors must reserve IDs explicitly and release them only
# after firmware confirms deletion. Failed allocation must not return ID zero.
allocator=topo.split('NTSTATUS SofIpc4AllocInstanceId(',1)[1].split('VOID SofIpc4FreeInstanceId(',1)[0]
assert '*OutInstanceId = 0xFFFFFFFFu;' in allocator
assert 'if (!mask) return STATUS_NOT_SUPPORTED;' in allocator
assert allocator.index('KeAcquireSpinLock') < allocator.index('*mask |=') < allocator.index('KeReleaseSpinLock')
for name in ('Ssp','Dmic'):
    body=topo.split('NTSTATUS SofIpc4Create'+name+'Pipeline(',1)[1].split('\n}',1)[0]
    assert 'return SofIpc4CreateNhltPipeline(' in body
for name in ('Playback','Capture','Hdmi','Nhlt','Alh'):
    body=topo.split('NTSTATUS SofIpc4Create'+name+'Pipeline(',1)[1].split('\n}',1)[0]
    assert body.count('status = SofIpc4AllocInstanceId(')==3
    for part in body.split('status = SofIpc4AllocInstanceId(')[1:]:
        assert part.split(';',1)[1].lstrip().startswith('if (!NT_SUCCESS(status)) goto cleanup;')
    paused=body[body.index('SOF_IPC4_PIPE_PAUSED'):body.index('*OutHostInst = hostInst')]
    assert 'if (!NT_SUCCESS(status))' in paused and 'goto cleanup;' in paused
    cleanup=body.split('cleanup:',1)[1]
    assert cleanup.index('if (NT_SUCCESS(SofIpc4DeletePipeline') < cleanup.index('SofIpc4FreeInstanceId')
    assert cleanup.count('SofIpc4FreeInstanceId(')==3
print('PASS: module ID allocation failures and pipeline deletion ownership checks')

# Gain commands share the tested single-block large config encoder.
volume=topo.split('NTSTATUS SofIpc4SetVolumeCurve(',1)[1].split('\n}',1)[0]
assert 'pj_ipc4_large_config_set(SofGraphWireId(Ctx, SOF_IPC4_MOD_ID_GAIN' in volume
assert 'return SofIpc4SendGraphPacket(Ctx, result, &packet);' in volume
assert 'GainInstanceId >= 256u' in volume
assert 'VolumeLinear > SOF_IPC4_VOLUME_ZERO_DB' in volume
print('PASS: gain configuration uses checked IPC4 single-block command')
