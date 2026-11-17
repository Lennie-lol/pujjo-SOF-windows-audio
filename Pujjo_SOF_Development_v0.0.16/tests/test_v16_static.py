# SPDX-License-Identifier: BSD-3-Clause
"""Source/WDK project contracts, explicitly not a Windows build."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
loader=(root/'src/driver/Loader.c').read_text()
start=loader.index('SofAllocateDmaBuffer(');end=loader.index('\n}',start)
allocate=loader[start:end]
assert 'WdfCommonBufferGetAlignedLogicalAddress' in allocate
assert 'MmAllocateContiguousNodeMemory' not in allocate and 'MmGetPhysicalAddress(' not in allocate
assert 'sizeof(struct sof_man4_module)' in loader
ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
tree=ET.parse(root/'sof_dsp.vcxproj')
assert all(x.text=='Driver' for x in tree.findall('.//m:ConfigurationType',ns))
assert all(x.text=='WindowsKernelModeDriver10.0' for x in tree.findall('.//m:PlatformToolset',ns))
assert not tree.findall('.//m:EntryPointSymbol',ns)
probe=(root/'src/driver/peripheral/PujjoPeripheralKmdf.c').read_text()
assert 'Pj5682sSpbInitialize(device' in probe
assert 'attributes.ExecutionLevel = WdfExecutionLevelPassive' in probe
assert 'RtlZeroMemory(ctx, sizeof(*ctx))' not in probe
assert 'Pj1019GpioOpen' not in probe and 'Pj1019GpioWrite' not in probe
print('PASS: WDF DMA policy, persistent transport locks, passive callbacks, WDK configuration')
