# Package verification — v0.0.16

SHA256SUMS.txt covers every packaged regular file except itself and SOURCE_SHA256_MANIFEST.json. SOURCE_SHA256_MANIFEST.json records the same set. Verify hashes after extraction. The ZIP includes complete source, updated reports and current/historical logs; it contains no new Windows binary or downloaded firmware. Temporary compiler products and Python caches are excluded.

validation-v0.0.16/input-comparison.json lists changed/added/removed paths relative to the attached v0.0.15 source ZIP. validation-v0.0.16/source-inventory.json inventories source/document files. Upstream integrity verification remains part of the offline suite.
