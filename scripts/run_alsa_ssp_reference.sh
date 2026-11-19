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
printf '%s\n' "$rev" > "$out/alsa-revision.txt"
