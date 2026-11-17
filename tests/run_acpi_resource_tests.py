#!/usr/bin/env python3
"""Static-only ACPI evidence checks: no Windows driver invocation."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='pujjo_acpi_') as temp:
    exe = Path(temp) / 'test_pujjo_acpi_resources'
    subprocess.run([
        os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
        str(root/'src/driver/experimental/PujjoAcpiResources.c'),
        str(root/'tests/test_pujjo_acpi_resources.c'), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: ACPI evidence validation test compiled')
