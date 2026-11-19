/* SPDX-License-Identifier: BSD-3-Clause */
/* Call the pinned upstream ALSA encoder, not a reimplementation.
 * The only local serialization is the ACPI envelope around the upstream
 * endpoint records. This program is a host evidence tool, not driver code. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intel-nhlt.h"
#include "ssp-nhlt.h"
#include "ssp/ssp-process.h"

static int emit(unsigned port, const char *path)
{
    struct intel_nhlt_params params = {0};
    struct endpoint_descriptor *ep[2] = {NULL, NULL};
    unsigned n = port == 0 ? 2u : 1u;
    uint32_t length = 37;
    unsigned char *table = NULL;
    FILE *out = NULL;
    int result = 1;
    int io = port == 0 ? 38400000 : 24576000;
    int mclk = port == 0 ? 19200000 : 24576000; /* SSP1 surrogate only */
    if (ssp_init_params(&params) < 0 ||
        ssp_set_params(&params, port == 0 ? "duplex" : "playback", (int)port,
                       io, port == 0 ? 384 : 0, 24, 0, 0, 0,
                       "false", NULL, 0x105) < 0 ||
        ssp_hw_set_params(&params, "I2S", "codec_mclk_in", "codec_consumer",
                          "false", "codec_consumer", "false", mclk,
                          port == 0 ? 2400000 : 3072000, 48000, 2,
                          port == 0 ? 25 : 32, 3, 3) < 0 ||
        ssp_calculate(&params) < 0) goto done;
    for (unsigned i = 0; i < n; ++i) {
        if (nhlt_ssp_get_ep(&params, &ep[i], 0, (uint8_t)i) < 0 ||
            !ep[i] || ep[i]->length > 4096u || ep[i]->length < 19u)
            goto done;
        length += ep[i]->length;
    }
    table = calloc(1, length);
    if (!table) goto done;
    memcpy(table, "NHLT", 4);
    for (unsigned i = 0; i < 4; ++i) table[4+i] = (uint8_t)(length >> (8*i));
    table[36] = (uint8_t)n;
    unsigned pos = 37;
    for (unsigned i = 0; i < n; ++i) {
        memcpy(table+pos, ep[i], ep[i]->length);
        pos += ep[i]->length;
    }
    unsigned sum = 0;
    for (unsigned i = 0; i < length; ++i) sum += table[i];
    table[9] = (uint8_t)(0u-sum);
    out = fopen(path, "wb");
    if (!out) goto done;
    size_t written = fwrite(table, 1, length, out);
    int closed = fclose(out);
    out = NULL;
    if (written != length || closed != 0) goto done;
    result = 0;
done:
    if (out) fclose(out);
    free(table);
    free(ep[0]);
    free(ep[1]);
    free(params.ssp_params);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: alsa_ssp_reference SSP0.nhlt SSP1.nhlt\n");
        return 2;
    }
    return emit(0, argv[1]) || emit(1, argv[2]);
}
