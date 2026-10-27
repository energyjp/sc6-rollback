#!/usr/bin/env python3
"""The sha256 of a built sc6rb.asi and its timestamp-normalized sha256 (the PE header's TimeDateStamp zeroed).

  python tools/asi_hash.py build/sc6rb.asi [more.asi ...]

The linker stamps the build time into the PE header, so two builds of the same source differ only there. Compare the normalized hash
with the one published for a release to check that your build matches it."""
import hashlib, struct, sys

for p in sys.argv[1:]:
    b = bytearray(open(p, 'rb').read())
    raw = hashlib.sha256(b).hexdigest()
    ts_at = struct.unpack_from('<I', b, 0x3c)[0] + 8   # e_lfanew + 8: IMAGE_FILE_HEADER.TimeDateStamp
    ts = struct.unpack_from('<I', b, ts_at)[0]
    b[ts_at:ts_at + 4] = b'\0\0\0\0'
    print('%s size=%d sha256=%s normalized=%s timedatestamp=0x%08x' % (p, len(b), raw, hashlib.sha256(b).hexdigest(), ts))
