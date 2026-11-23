#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Host-only Windows DMIC INF and staging contracts. Never installs anything."""
from pathlib import Path

root=Path(__file__).resolve().parents[1]
full=(root/'src/driver/pujjo_audio.inx').read_text()
dmic=(root/'packaging/review/pujjo_dmic_only.inx').read_text()
stage=(root/'scripts/Stage-PujjoDmicReview.ps1').read_text()
verify=(root/'scripts/Verify-PujjoDmicReview.ps1').read_text()
workflow=(root/'.github/workflows/compile-windows.yml').read_text()
bus=(root/'src/driver/PujjoAudioBus.c').read_text()
device=(root/'src/driver/Device.c').read_text()
port=(root/'src/driver/PortClsAdapter.cpp').read_text()

for candidate in (dmic,full):
    assert 'Include=ks.inf,wdmaudio.inf' in candidate
    assert 'Needs=KS.Registration,WDMAUDIO.Registration' in candidate
    assert 'AddInterface=%Topology%' in candidate
assert dmic.count('PUJJO\\AudioDmic')==1
for name in ('AudioHeadphone','AudioHeadsetMic','AudioSpeaker','PCI\\VEN_8086'):
    assert name not in dmic
for name in (
    'AddService=pujjo_audio,0x00000002,Service',
    'AddInterface=%Audio%,DmicWave,WaveInterface',
    'AddInterface=%Capture%,DmicWave,WaveInterface',
    'AddInterface=%Realtime%,DmicWave,WaveInterface',
    'AddInterface=%Audio%,DmicTopology,TopologyInterface',
    'AddInterface=%Topology%,DmicTopology,TopologyInterface',
    'Topology="{DDA54A40-1E4C-11D1-A050-405705C10000}"',
    'CatalogFile=pujjo_audio.cat'
):
    assert name in dmic,name
assert 'WdfFdoAddStaticChild(Ctx->WdfDevice, child)' in bus
assert 'Ctx->FwState != SOF_FW_BOOT_COMPLETE' in bus
assert 'g_CanaryArmed && ctx->ArmedTesting != 0 && ctx->EnablePortCls != 0' in device
assert 'PcRegisterSubdevice' in port and 'PcRegisterPhysicalConnection' in port
for name in ('Stage-PujjoDmicReview.ps1','Verify-PujjoDmicReview.ps1',
             'Pujjo-DMIC-endpoint-UNSIGNED-REVIEW'):
    assert name in workflow
assert 'InfVerif.exe' in stage and 'Inf2Cat.exe' in stage
assert 'Get-FileHash' in stage and 'UNSIGNED-DMIC-REVIEW-ONLY.txt' in stage
assert 'Source revision' in verify and 'Get-FileHash' in verify
assert '$hashRecords = @(' in verify and '$matches=' not in verify.lower()
assert '[regex]::IsMatch' in verify and '$record.Algorithm' in verify
assert verify.count("Write-Output 'PASS:") == 1
assert verify.count("$inf = Get-Content") == 1
assert verify.count("foreach ($name in $names)") == 2
assert "Get-AuthenticodeSignature" in verify
for command in ('pnputil','bcdedit','Set-ItemProperty','Import-Certificate','signtool sign'):
    assert command.lower() not in stage.lower()
    assert command.lower() not in verify.lower()
print('PASS: PortCls core registration, isolated DMIC review packaging, source and FW_READY gates')
