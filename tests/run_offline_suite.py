#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Explicit hardware-free allowlist. Retains every output and propagates failures."""
import argparse, json, os, re, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SUITES=[
 'tests/test_kmdf_security.py','tests/test_bringup_static.py',
 'tests/run_bringup_tests.py','tests/test_pujjo_sof_fw_static.py',
 'tests/run_pujjo_sof_fw_tests.py','tests/test_rt1019p_static.py',
 'tests/run_rt1019p_amp_tests.py','tests/test_rt5682s_static.py',
 'tests/run_rt5682s_codec_tests.py','tests/test_acpi_parser.py',
 'tests/run_acpi_resource_tests.py','tests/run_device_map_tests.py',
 'tests/run_pujjo_tests.py','tests/run_codec_init_tests.py',
 'tests/run_headset_clock_tests.py','tests/verify_linux_topology.py',
 'scripts/verify_upstream_integrity.py','tests/test_sof_zephyr_log.py',
 'tests/run_resource_policy_tests.py','tests/run_loader_transport_tests.py',
 'tests/test_audit_regressions.py','tests/run_windows_transport_tests.py',
 'tests/test_v16_static.py','tests/run_dma_tests.py',
 'tests/test_windows_ci_v017.py','tests/run_kernel_mock_abi_tests.py',
 'tests/test_preinstall_guards.py','tests/run_ipc4_boot_model_tests.py',
 'tests/run_boot_speaker_integration_tests.py',
 'tests/run_ipc4_protocol_tests.py','tests/test_ipc4_protocol_static.py',
 'tests/test_ipc4_evidence_collector_static.py',
 'tests/test_ipc4_pretest_safety.py',
]
def main():
    p=argparse.ArgumentParser();p.add_argument('--logs',default='validation-v0.0.17/final')
    args=p.parse_args();logs=Path(args.logs).resolve();logs.mkdir(parents=True,exist_ok=True)
    results=[]
    for script in SUITES:
        cmd=[sys.executable,script]
        try:
            run=subprocess.run(cmd,cwd=ROOT,text=True,encoding='utf-8',errors='replace',
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
            output=run.stdout;rc=run.returncode
            status='FAIL' if rc else 'PASS'
            if not rc and re.search(r'(?im)^SKIP:',output): status='SKIP'
        except (OSError,subprocess.TimeoutExpired) as e:
            output=str(e);rc=None;status='FAIL'
        (logs/(Path(script).stem+'.log')).write_text('COMMAND: '+ ' '.join(cmd)+'\n'+output,encoding='utf-8')
        skipped=re.search(r'OK \(skipped=(\d+)\)',output)
        results.append(dict(command=cmd,status=status,exit_code=rc,
                            skipped_cases=int(skipped.group(1)) if skipped else 0))
        print(status,script,flush=True)
    results += [
      dict(command=['python','tests/run_topology_evidence_tests.py'],status='SKIP',reason='External topology archive not bundled; test separately when available'),
      dict(command=['python','tests/verify_kernel_reference.py'],status='SKIP',reason='External Linux kernel archive not bundled; test separately when available'),
      dict(command=['python','tests/test_s0idle_and_d3.py'],status='NOT RUN',reason='Physical DUT test forbidden'),
      dict(command=['python','tests/run_regression_suite.py'],status='NOT RUN',reason='Physical DUT test forbidden'),
    ]
    (logs/'summary.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    failed=sum(r['status']=='FAIL' for r in results)
    print(f'{len(SUITES)} executed commands; {failed} failed. See summary.json and logs for skips.')
    return int(failed!=0)
if __name__=='__main__': sys.exit(main())
