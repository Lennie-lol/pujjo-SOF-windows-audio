# SPDX-License-Identifier: BSD-3-Clause
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from prepare_pujjo_ssp_candidate import (
    ENDPOINT, WAVE, PCM, build, compare, decode, digest, pinned, reference_records,
)


def blob(port, source=0, divider=0):
    # Synthetic fixtures: these are not hardware-approved register images.
    words = [0] * 24
    words[1:3] = [0xee000105, 96]
    words[11] = 7 | (1 << 20) | ((15 if port == 0 else 7) << 8)
    words[14] = (25 if port == 0 else 32) << 16
    words[15:17] = [3, 3]
    words[21:24] = [1 | source, 1, divider]
    return struct.pack("<24I", *words)


def table(port, data, dirs=None):
    bits = 25 if port == 0 else 32
    align = 2 * bits // 8
    fmt = WAVE.pack(0xfffe, 2, 48000, 48000 * align, align,
                    bits, 22, bits, 0, bytes(16))
    body = struct.pack("<I", 2) + bytes(2) + bytes([1]) + fmt
    body += struct.pack("<I", len(data)) + data
    dirs = ({0, 1} if port == 0 else {0}) if dirs is None else dirs
    eps = b"".join(ENDPOINT.pack(ENDPOINT.size + len(body), 3, 0, 0x8086,
                  0xAE34, 0, 0, 0, d, port) + body for d in sorted(dirs))
    return b"NHLT" + struct.pack("<I", 37 + len(eps)) + bytes(28) + bytes([len(dirs)]) + eps


class Prepare(unittest.TestCase):
    def test_fixed_candidate_geometry(self):
        from generate_pujjo_ssp_candidate import candidate
        a, b = candidate(0), candidate(1)
        self.assertEqual(len(a), 96)
        self.assertEqual(len(b), 96)
        for port, data, expected_source, expected_padding in ((0, a, 0, 1), (1, b, 1, 8)):
            d = decode(data, port)
            self.assertEqual(d["mdivctlr"], 1 | (expected_source << 20))
            self.assertEqual(d["mdivr"], 0)
            padding = ((d["sspsp"] >> 23) & 3) | (((d["sspsp"] >> 26) & 7) << 2)
            self.assertEqual(padding, expected_padding)
        with self.assertRaises(ValueError):
            candidate(2)

    def test_clock_plan_audit(self):
        from generate_pujjo_ssp_candidate import candidate, audit_clock_plan
        for port, expected in ((0, 2400000), (1, 3072000)):
            data = candidate(port)
            report = audit_clock_plan(data, port)
            self.assertEqual(report["bclk_hz"], expected)
            self.assertEqual(report["mclk_hz"], 19200000)
            self.assertFalse(report["hardware_qualified"])
            for word, bit in ((21, 16), (21, 20), (21, 1), (23, 0),
                              (11, 6), (11, 8), (14, 23)):
                changed = bytearray(data)
                changed[word*4 + bit//8] ^= 1 << (bit%8)
                with self.subTest(port=port, word=word, bit=bit), self.assertRaises(ValueError):
                    audit_clock_plan(changed, port)

    def test_windows_metadata_and_payload(self):
        a, b = blob(0), blob(1)
        result, report = build(table(0, a), table(1, blob(1, 0x10000, 0xfff)), a, b)
        self.assertEqual(len(report["records"]), 3)
        self.assertEqual(sum(result) & 255, 0)
        self.assertEqual(report["nhlt_sha256"], digest(result))
        self.assertEqual(result[36], 3)
        pos = 37
        for port, direction, expected in [(0, 0, a), (0, 1, a), (1, 0, b)]:
            desc = ENDPOINT.unpack_from(result, pos)
            self.assertEqual(desc[-3:], (4, direction, port))
            start = pos + ENDPOINT.size + 7
            self.assertEqual(WAVE.unpack_from(result, start),
                (0xfffe, 2, 48000, 384000, 8, 32, 22, 24, 3, PCM))
            size = struct.unpack_from("<I", result, start + WAVE.size)[0]
            self.assertEqual(size, 96)
            self.assertEqual(result[start+44:start+44+size], expected)
            pos += desc[0]
        self.assertEqual(pos, len(result))
        self.assertEqual(set(report["records"][2]["clock_differences"]), {"mdivctlr", "mdivr"})

    def test_rev3_exact_table_audit(self):
        from audit_pujjo_rev3_nhlt import audit
        from generate_pujjo_ssp_candidate import candidate
        a,b=candidate(0),candidate(1)
        data,_=build(table(0,a),table(1,b),a,b)
        self.assertEqual(len(audit(data)),3)
        def changed(index):
            bad=bytearray(data)
            bad[index]^=1
            bad[9]=0
            bad[9]=-sum(bad)&255
            return bytes(bad)
        for offset in (36,37,37+4,37+ENDPOINT.size,
                       37+ENDPOINT.size+7+20,37+ENDPOINT.size+7+WAVE.size,
                       len(data)-1):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                audit(changed(offset))
        with self.assertRaises(ValueError):
            audit(data[:-1])
        with self.assertRaises(ValueError):
            audit(data+bytes(1))
        # Repaired checksum does not legalize duplicate routes.
        bad=bytearray(data)
        ep=struct.unpack_from("<I",data,37)[0]
        bad[37+ep:37+2*ep]=bad[37:37+ep]
        bad[9]=0
        bad[9]=-sum(bad)&255
        with self.assertRaises(ValueError):
            audit(bytes(bad))

    def test_reject_nonclock_changes(self):
        base = blob(0)
        for index in (0, 3, 12, 17, 20, 21, 22, 23):
            altered = bytearray(base)
            altered[4*index+3] ^= 0x80
            with self.assertRaises(ValueError):
                compare(base, altered, 0)
        with self.assertRaises(ValueError):
            compare(base, blob(0, 0, 1), 0)
        with self.assertRaises(ValueError):
            decode(blob(1), 0)

    def test_untrusted_tables_and_digests(self):
        ref = table(0, blob(0))
        self.assertEqual(set(reference_records(ref, 0)), {0, 1})
        for bad in (ref[:-1], ref+b"x", table(0, blob(0), [0]),
                    table(0, blob(0), [0, 0, 1])):
            with self.assertRaises(ValueError):
                reference_records(bad, 0)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.bin"
            path.write_bytes(ref)
            self.assertEqual(pinned(path, digest(ref), len(ref)), ref)
            with self.assertRaises(ValueError):
                pinned(path, "0"*64, len(ref))
            with self.assertRaises(ValueError):
                pinned(path, digest(ref), len(ref)-1)

    def test_pipeline_quarantines_failed_deletion(self):
        src = (ROOT / "src/driver/Topo.c").read_text()
        src = src.split("static NTSTATUS SofIpc4CreateNhltPipeline(", 1)[1].split(
            "NTSTATUS SofIpc4CreateSspPipeline(", 1)[0]
        self.assertIn("if (OutDaiInst) *OutDaiInst = 0xFFFFFFFFu;", src)
        self.assertIn("Ctx->AudioDmaFaulted || Ctx->FwState", src)
        self.assertIn("(configBytes & 3u)", src)
        cleanup = src.split("cleanup:", 1)[1]
        self.assertLess(cleanup.index("SofIpc4DeletePipeline"), cleanup.index("Ctx->AudioDmaFaulted = TRUE"))


if __name__ == "__main__":
    unittest.main()
