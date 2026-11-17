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
