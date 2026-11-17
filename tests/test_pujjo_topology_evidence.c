/* SPDX-License-Identifier: BSD-3-Clause */
#include "../src/driver/experimental/PujjoTopologyEvidence.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load_file(const char *name, size_t *n)
{
    FILE *f = fopen(name, "rb");
    unsigned char *data;
    long length;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    *n = (size_t)length;
    data = (unsigned char*)malloc(*n);
    if (!data || fread(data, 1, *n, f) != *n) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    return data;
}
int main(int argc, char **argv)
{
    pj_topology_evidence evidence;
    size_t size;
    unsigned char *data;
    if (argc != 2) { fprintf(stderr, "Usage: test_pujjo_topology_evidence FILE.tplg\n"); return 2; }
    data = load_file(argv[1], &size);
    assert(data);
    assert(pj_inspect_adl_topology(data, size, &evidence) == PJ_TPLG_VALID);
    assert(evidence.section_count > 10);
    assert(evidence.has_ssp1_speaker_playback);
    assert(evidence.has_ssp0_headset_playback);
    assert(evidence.has_ssp2_bt_playback);
    printf("PASS: topology %u sections, SSP1 speaker/SSP0 headset/SSP2 BT markers\n", evidence.section_count);
    /* Wrong header, truncated section, and missing required routes must fail closed. */
    data[0] = 'X';
    assert(pj_inspect_adl_topology(data, size, &evidence) == PJ_TPLG_BAD_HEADER);
    data[0] = 'C';
    assert(pj_inspect_adl_topology(data, size - 1, &evidence) == PJ_TPLG_TRUNCATED);
    assert(pj_inspect_adl_topology(data, 0, &evidence) == PJ_TPLG_INVALID_INPUT);
    assert(pj_inspect_adl_topology(NULL, size, &evidence) == PJ_TPLG_INVALID_INPUT);
    /* Change SSP1 marker in all copies: topology should be rejected. */
    for (size_t i = 0; i + 8 < size; ++i) {
        if (memcmp(data + i, "SSP1.OUT", 8) == 0) data[i] = 'X';
    }
    assert(pj_inspect_adl_topology(data, size, &evidence) == PJ_TPLG_MISSING_ROUTE);
    free(data);
    puts("PASS: malformed and incompatible topology rejection");
    return 0;
}
