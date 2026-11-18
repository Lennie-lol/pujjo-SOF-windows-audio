#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Portable host C checks, sanitizer execution and GCC static analysis.
Never compiles a Windows kernel or accesses hardware. Exact commands retained.
"""
import json, os, shlex, shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CASES=[
 ('board','src/driver/PujjoBoardId.c','tests/test_pujjo_boardid.c',[]),
 ('resources','src/driver/peripheral/PujjoResourcePolicy.c','tests/test_pujjo_resource_policy.c',[]),
 ('acpi','src/driver/experimental/PujjoAcpiResources.c','tests/test_pujjo_acpi_resources.c',[]),
 ('map','src/driver/experimental/PujjoDeviceMap.c','tests/test_pujjo_device_map.c',[]),
 ('clock','src/driver/experimental/PujjoHeadsetClock.c','tests/test_pujjo_headset_clock.c',[]),
 ('firmware','src/driver/experimental/PujjoSofFirmwarePolicy.c','tests/test_pujjo_sof_fw_policy.c',[]),
 ('catalog','src/driver/PujjoFirmwareCatalog.c','tests/test_pujjo_firmware_catalog.c',[]),
 ('profile-cache','src/driver/PujjoFirmwareCatalog.c','tests/test_pujjo_profile_cache.c',[]),
 ('codec-locked','src/driver/experimental/PujjoRt5682sCodec.c','tests/test_rt5682s_codec.c',[]),
 ('codec-mock','src/driver/experimental/PujjoRt5682sCodec.c','tests/test_rt5682s_codec.c',['-DPJ5682S_TEST_ONLY_WRITES']),
 ('amp-locked','src/driver/experimental/PujjoRt1019pAmp.c','tests/test_rt1019p_amp.c',[]),
 ('amp-mock','src/driver/experimental/PujjoRt1019pAmp.c','tests/test_rt1019p_amp.c',['-DPJ1019_TEST_ONLY_CALLBACKS','-DPJ1019_TEST_BUILD']),
 ('init-locked','src/driver/experimental/PujjoAudioInit.c','tests/test_pujjo_audio_init.c',[]),
 ('init-mock','src/driver/experimental/PujjoAudioInit.c','tests/test_pujjo_audio_init.c',['-DPJ_AUDIO_ENABLE_EXPERIMENTAL_IO=1']),
]
def main():
    logs=ROOT/'validation-v0.0.17/matrix';logs.mkdir(parents=True,exist_ok=True)
    results=[]
    (logs/'environment.log').write_text('ASAN_OPTIONS='+os.environ.get('ASAN_OPTIONS','')+'\n')
    for compiler in ('gcc','clang'):
        cc=shutil.which(compiler)
        if not cc:
            results.append(dict(compiler=compiler,status='SKIP',reason='Compiler not installed'));continue
        profiles=[('strict',['-O2']),('sanitizers',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-sanitize-recover=all'])]
        if compiler=='gcc': profiles.append(('analyzer',['-O0','-fanalyzer']))
        # Non-PIE sanitizer executables avoid Linux ASan shadow-address collisions.
        if sys.platform.startswith('linux'): profiles[1][1].append('-no-pie')
        for profile,flags in profiles:
            with tempfile.TemporaryDirectory(prefix='pujjo_matrix_') as d:
                for name,source,test,defines in CASES:
                    exe=Path(d)/(name+('.exe' if os.name=='nt' else ''))
                    cmd=[cc,'-std=c11','-Wall','-Wextra','-Werror','-pedantic',*flags,*defines,
                         '-I'+str(ROOT/'src/driver/peripheral'),str(ROOT/source),str(ROOT/test),'-o',str(exe)]
                    run=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
                    output='COMMAND: '+shlex.join(cmd)+'\n'+run.stdout+run.stderr
                    rc=run.returncode
                    if rc==0:
                        run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
                        output+='COMMAND: '+str(exe)+'\n'+run.stdout+run.stderr;rc=run.returncode
                    status='PASS' if rc==0 else 'FAIL'
                    label=f'{compiler}-{profile}-{name}'
                    (logs/(label+'.log')).write_text(output,encoding='utf-8')
                    results.append(dict(test=label,status=status,exit_code=rc))
                    print(status,label,flush=True)
                # Execute full transport sources and coordinator as well as parser bodies.
                for runner in ('run_loader_transport_tests.py','run_windows_transport_tests.py','run_bringup_tests.py','run_dma_tests.py'):
                    env=dict(os.environ,CC=cc,CFLAGS=shlex.join(flags))
                    cmd=[sys.executable,'tests/'+runner]
                    run=subprocess.run(cmd,cwd=ROOT,env=env,capture_output=True,text=True,timeout=120)
                    label=f'{compiler}-{profile}-{Path(runner).stem}'
                    (logs/(label+'.log')).write_text('COMMAND: '+shlex.join(cmd)+'\nCFLAGS: '+env['CFLAGS']+'\n'+run.stdout+run.stderr)
                    results.append(dict(test=label,status='PASS' if run.returncode==0 else 'FAIL',exit_code=run.returncode))
                    print(results[-1]['status'],label,flush=True)
    (logs/'summary.json').write_text(json.dumps(results,indent=2))
    return int(any(r['status']=='FAIL' for r in results))
if __name__=='__main__':sys.exit(main())
