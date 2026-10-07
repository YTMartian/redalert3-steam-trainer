# -*- coding: utf-8 -*-
"""Pack unit_images/*.png into one blob the overlay DLL links in."""
from __future__ import print_function

import os
import struct
import sys

MAGIC = 0x474D4955  # 'UIMG' little-endian
VERSION = 1


def main():
    if len(sys.argv) != 3:
        print('usage: pack_unit_images.py <unit_images dir> <output dir>')
        return 1
    src = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2])
    if not os.path.isdir(src):
        print('unit_images not found: %s' % src)
        return 1
    os.makedirs(out_dir, exist_ok=True)

    entries = []
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        for name in sorted(filenames):
            lower = name.lower()
            if not (lower.endswith('.png') or lower.endswith('.webp')):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, src).replace('\\', '/')
            if lower.endswith('.webp'):
                try:
                    import io
                    from PIL import Image
                    image = Image.open(full).convert('RGBA')
                    buf = io.BytesIO()
                    image.save(buf, format='PNG')
                    data = buf.getvalue()
                except Exception as exc:
                    print('skip %s (%s)' % (rel, exc))
                    continue
                rel = rel[:-5] + '.png'
            else:
                with open(full, 'rb') as handle:
                    data = handle.read()
            entries.append((rel.encode('utf-8'), data))

    blob = bytearray()
    blob += struct.pack('<III', MAGIC, VERSION, len(entries))
    for rel, data in entries:
        blob += struct.pack('<H', len(rel))
        blob += rel
        blob += struct.pack('<I', len(data))
        blob += data

    pack_path = os.path.join(out_dir, 'unit_images_pack.bin')
    rc_path = os.path.join(out_dir, 'unit_images.rc')
    with open(pack_path, 'wb') as handle:
        handle.write(blob)
    with open(rc_path, 'w', newline='\n') as handle:
        handle.write('1 RCDATA "unit_images_pack.bin"\n')
    print('packed %d images, %d bytes' % (len(entries), len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
