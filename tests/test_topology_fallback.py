# SPDX-License-Identifier: BSD-3-Clause
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from build_pujjo_topology_config import extract


def block(private):
    manifest = struct.pack("<I", 112) + bytes(104) + struct.pack("<I", len(private)) + private
    return struct.pack("<9I", 0x41536f43, 5, 0, 8, 36, 0, len(manifest), 0, 1) + manifest


def read(data):
    return extract(data, hashlib.sha256(data).hexdigest())


class TopologyFallback(unittest.TestCase):
    def test_manifest(self):
        table = b"NHLT" + struct.pack("<I", 37) + bytes(29)
        item = struct.pack("<II", 1, len(table)) + table
        data = block(struct.pack("<4H", 3, 29, 1, 1) + item)
        result = read(data)
        self.assertEqual(sum(result) & 255, 0)
        self.assertEqual(table[10:], result[10:])
        for invalid in (data[:-1], data + data, block(bytes([3, 23, 0])),
                        block(struct.pack("<4H", 3, 29, 1, 2) + item + item)):
            with self.assertRaises(ValueError):
                read(invalid)
        with self.assertRaises(ValueError):
            extract(data, "0" * 64)


if __name__ == "__main__":
    unittest.main()
