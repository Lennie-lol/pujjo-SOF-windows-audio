#!/usr/bin/env python3
"""Compile test-only Pujjo audio init coordinator. Never touches physical hardware."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parent.parent
cc = os.environ.get('CC', 'cc')
src = root / 'src' / 'driver' / 'experimental' / 'PujjoAudioInit.c'
test = root / 'tests' / 'test_pujjo_audio_init.c'
with tempfile.TemporaryDirectory(prefix='pujjo_audio_test_') as d:
    for title, flag in [('locked', []), ('mock', ['-DPJ_AUDIO_ENABLE_EXPERIMENTAL_IO=1'])]:
        exe = Path(d) / title
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        *flag, str(src), str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(f'PASS: {title} build')
