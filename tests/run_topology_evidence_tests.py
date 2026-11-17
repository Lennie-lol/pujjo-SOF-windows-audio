#!/usr/bin/env python3
"""Tests a portable topology validator with the *user-supplied* Linux audio ZIP.
No driver installation, no output to hardware and no firmware distribution.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

root = Path(__file__).resolve().parent.parent
archive = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/mnt/data/chromebook-linux-audio-main.zip')
member = 'chromebook-linux-audio-main/blobs/adl/sof-adl-rt1019-rt5682.tplg'
if not archive.exists():
    raise SystemExit(f'Provide path to chromebook-linux-audio-main.zip: {archive}')
with zipfile.ZipFile(archive) as z:
    binary = z.read(member)
print(f'Topology from Linux audio ZIP, size {len(binary)} B, SHA256 {hashlib.sha256(binary).hexdigest()}')
with tempfile.TemporaryDirectory(prefix='pujjo_tplg_') as tmp:
    tmp = Path(tmp)
    path = tmp / 'linux_reference.tplg'
    path.write_bytes(binary)
    exe = tmp / ('test_topology.exe' if os.name == 'nt' else 'test_topology')
    cc = os.environ.get('CC', 'cc')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    str(root/'src/driver/experimental/PujjoTopologyEvidence.c'),
                    str(root/'tests/test_pujjo_topology_evidence.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(path)], check=True)
print('PASS: topology reader tested; no hardware accessed')
