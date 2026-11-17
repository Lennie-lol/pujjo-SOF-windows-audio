/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/driver/experimental/PujjoSofFirmwarePolicy.h"
int main(void) {
    char path[128];
    char tiny[8] = {'x','x','x','x','x','x','x','x'};
    PJ_SOF_FW_EVIDENCE evidence = {0};
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC_UNKNOWN,path,sizeof path)
            == PJ_SOF_FW_UNVERIFIED_PROTOCOL && path[0]=='\0');
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC4,path,sizeof path)
            == PJ_SOF_FW_OK);
    assert(strcmp(path,"sof-ipc4/adl-n/intel-signed/sof-adl-n.ri")==0);
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC3,path,sizeof path)
            == PJ_SOF_FW_OK);
    assert(strcmp(path,"sof/intel-signed/sof-adl-n.ri")==0);
    assert(pj_sof_candidate_firmware(0x8086,0xa0c8,PJ_SOF_IPC4,path,sizeof path)
            == PJ_SOF_FW_UNSUPPORTED_BOARD && path[0]=='\0');
    assert(pj_sof_candidate_firmware(0x1234,0x54c8,PJ_SOF_IPC4,path,sizeof path)
            == PJ_SOF_FW_UNSUPPORTED_BOARD && path[0]=='\0');
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,(PJ_SOF_IPC)100,path,sizeof path)
            == PJ_SOF_FW_UNVERIFIED_PROTOCOL && path[0]=='\0');
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC4,tiny,sizeof tiny)
            == PJ_SOF_FW_NO_SPACE && tiny[0]=='\0');
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC4,NULL,sizeof path)
            == PJ_SOF_FW_INVALID_ARGUMENT);
    assert(pj_sof_candidate_firmware(0x8086,0x54c8,PJ_SOF_IPC4,path,0)
            == PJ_SOF_FW_INVALID_ARGUMENT);
    assert(!pj_sof_evidence_complete(NULL));
    assert(!pj_sof_evidence_complete(&evidence));
    evidence.board_identity_confirmed=1;
    evidence.ipc_protocol_measured=1;
    evidence.firmware_signature_checked=1;
    evidence.firmware_format_verified=1;
    evidence.firmware_dsp_compatibility_tested=1;
    assert(!pj_sof_evidence_complete(&evidence));
    evidence.firmware_origin_verified=1;
    assert(pj_sof_evidence_complete(&evidence));
    puts("PASS: SOF firmware candidates and fail-closed evidence policy");
    return 0;
}
