/* SPDX-License-Identifier: BSD-3-Clause */
/* Offline reference generator: do not load its output into hardware. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "intel-nhlt.h"
#include "dmic/dmic-process.h"

int main(int argc, char **argv)
{
    struct intel_nhlt_params nhlt = {0};
    uint32_t rate = 0, bits = 0, snr = 0, sensitivity = 0;
    uint16_t channels = 0;
    uint8_t array = 0, mics = 0, extension = 0;
    size_t length = 0;
    uint8_t *blob = NULL;
    FILE *file = NULL;
    int rc = 1;
    if (argc != 2) return 2;
    /* Recovery DMIC0 tokens; 38.4 MHz is the SOF v2.12 TGL/ADL
     * reference IO clock, not a measured clock on the target board.
     * Do not configure the separate 16 kHz wake-on-voice FIFO. */
    if (dmic_init_params(&nhlt) ||
        dmic_set_params(&nhlt, 0, 1, 38400000, 2, 32,
                        2400000, 4800000, 40, 60, 48000, 200))
        goto done;
    for (int i = 0; i < 2; ++i)
        if (dmic_set_pdm_params(&nhlt, i, 1, 1, 0, 0, 0, 0))
            goto done;
    if (dmic_calculate(&nhlt) ||
        dmic_get_vendor_blob_count(&nhlt) != 1 ||
        dmic_get_params(&nhlt, 0, &rate, &channels, &bits, &array,
                        &mics, &extension, &snr, &sensitivity) ||
        rate != 48000 || channels != 4 || bits != 32 ||
        dmic_get_vendor_blob_size(&nhlt, &length) ||
        length < 232 || length > 8192 || length % 4)
        goto done;
    blob = malloc(length);
    if (!blob || dmic_get_vendor_blob(&nhlt, blob))
        goto done;
    file = fopen(argv[1], "wb");
    if (!file) goto done;
    if (fwrite(blob, 1, length, file) != length) goto done;
    if (fclose(file)) { file = NULL; goto done; }
    file = NULL;
    printf("DMIC0 four-channel reference: %zu bytes\n", length);
    rc = 0;
done:
    if (file) fclose(file);
    free(blob);
    free(nhlt.dmic_params);
    return rc;
}
