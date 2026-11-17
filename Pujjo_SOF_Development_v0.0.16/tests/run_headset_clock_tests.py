"""BSD-3-Clause. Compile and run isolated non-hardware C unit tests."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
    output=Path(d)/('headsetclock.exe' if os.name=='nt' else 'headsetclock')
    cmd=[os.environ.get('CC', 'cc'),'-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/test_pujjo_headset_clock.c'),str(ROOT/'src/driver/experimental/PujjoHeadsetClock.c'),'-o',str(output)]
    print(' '.join(cmd));subprocess.run(cmd,check=True)
    subprocess.run([str(output)],check=True)
