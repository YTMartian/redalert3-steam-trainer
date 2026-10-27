# -*- coding: utf-8 -*-
"""Pack the unit name tables into the overlay DLL as one RCDATA blob.

The two tables used to travel beside the DLL (unit_names_csf.txt / unit_names.txt),
so every install showed three loose data files next to the injector. They are
build-time data, not user files, so they now live inside ra3_overlay_v4.dll and
the folder keeps only the exe plus the two DLLs.

Blob layout (little-endian):
    u32 magic 'UNAM', u32 version, u32 count
    per entry: u16 name_len, name bytes, u32 len, data bytes
"""
from __future__ import print_function

import os
import struct
import sys

MAGIC = 0x4D414E55  # 'UNAM' little-endian
VERSION = 1

# Resource name inside the blob -> file on disk. Order matters only for logging.
FILES = (
    ('csf', 'unit_names_csf.txt'),
    ('user', 'unit_names.txt'),
)


def main():
    if len(sys.argv) != 3:
        print('usage: pack_name_tables.py <dir with name tables> <output dir>')
        return 1
    src = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2])
    os.makedirs(out_dir, exist_ok=True)

    entries = []
    for key, name in FILES:
        path = os.path.join(src, name)
        if not os.path.isfile(path):
            print('missing %s (build the table first)' % path)
            return 1
        with open(path, 'rb') as handle:
            entries.append((key.encode('ascii'), handle.read()))

    blob = bytearray()
    blob += struct.pack('<III', MAGIC, VERSION, len(entries))
    for key, data in entries:
        blob += struct.pack('<H', len(key))
        blob += key
        blob += struct.pack('<I', len(data))
        blob += data

    pack_path = os.path.join(out_dir, 'name_tables_pack.bin')
    rc_path = os.path.join(out_dir, 'name_tables.rc')
    with open(pack_path, 'wb') as handle:
        handle.write(blob)
    # Resource id 2, type 10 (RCDATA). Id 1 is the unit icon pack.
    with open(rc_path, 'w', newline='\n') as handle:
        handle.write('2 RCDATA "name_tables_pack.bin"\n')
    print('packed %d name tables, %d bytes' % (len(entries), len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
