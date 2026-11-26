#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Compile actual ALSA code on a disposable Linux host with libasound2-dev.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
upstream="$(realpath "$1")"
out="$(realpath -m "$2")"
rev=f04b9e0f1285fe25d4146906c5a8511741b8abad
test "$(git -C "$upstream" rev-parse HEAD)" = "$rev"
test -z "$(git -C "$upstream" status --porcelain --untracked-files=no)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out"
cat > "$tmp/aconfig.h" <<'EOF'
#ifndef ATTRIBUTE_UNUSED
#define ATTRIBUTE_UNUSED __attribute__((unused))
#endif
EOF
nhlt="$upstream/topology/nhlt/intel"
# Unused ALSA text-config functions are discarded; the encoder math and
# endpoint serialization are unchanged upstream sources.
cc -std=gnu11 -O2 -ffunction-sections -fdata-sections \
    -I"$tmp" -I"$nhlt" \
    "$root/tests/alsa_ssp_reference.c" "$nhlt/ssp-nhlt.c" \
    "$nhlt/ssp/ssp-process.c" "$nhlt/ssp/ssp-debug.c" \
    -Wl,--gc-sections -lasound -o "$tmp/encoder"
"$tmp/encoder" "$out/ssp0-reference.nhlt" "$out/ssp1-reference.nhlt"
python3 "$root/scripts/generate_pujjo_ssp_candidate.py" \
    "$out/ssp0-reference.nhlt" "$out/ssp1-reference.nhlt" "$out"
python3 "$root/tests/run_nhlt_bounds_tests.py" --candidate-dir "$out"
python3 "$root/scripts/audit_pujjo_rev3_nhlt.py" "$out"
printf '%s\n' "$rev" > "$out/alsa-revision.txt"

# Independently generate a standalone DMIC0 review payload with untouched
# upstream filter and register calculations. No approved NHLT is produced.
cc -std=gnu11 -O2 -ffunction-sections -fdata-sections \
    -I"$tmp" -I"$nhlt" \
    "$root/tests/alsa_dmic_reference.c" "$nhlt/dmic/dmic-process.c" \
    "$nhlt/dmic/dmic-debug.c" -Wl,--gc-sections -lasound \
    -o "$tmp/dmic-encoder"
mkdir -p "$out/dmic"
"$tmp/dmic-encoder" "$out/dmic/dmic0-reference.bin"
"$tmp/dmic-encoder" "$tmp/dmic0-second.bin"
cmp "$out/dmic/dmic0-reference.bin" "$tmp/dmic0-second.bin"
python3 "$root/scripts/audit_pujjo_dmic_reference.py" \
    "$out/dmic/dmic0-reference.bin" "$out/dmic/dmic0-review.json"

python3 "$root/scripts/assemble_pujjo_complete_nhlt.py" "$out"
python3 "$root/scripts/generate_pujjo_dmic_header.py" "$out/dmic/dmic-only-candidate.nhlt" \
    "$root/src/driver/PujjoDmicNhltBytes.h" --check
python3 "$root/tests/run_nhlt_bounds_tests.py" --candidate-dir "$out" --complete

# The actual production validator must also accept the independently generated
# configuration, not just synthetic test bytes.
cc -std=c11 -Wall -Wextra -Werror -O2 \
    "$root/src/driver/PujjoDmicProfile.c" "$root/tests/test_pujjo_dmic_profile.c" \
    -o "$tmp/dmic-profile"
"$tmp/dmic-profile" "$out/dmic/dmic0-reference.bin" | tee "$out/dmic/production-validation.txt"
