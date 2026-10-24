import struct

def discover(path):
    d = open(path, 'rb').read()
    lo = 0x400000
    lit = b'AttributeModifier_InvulnerabilityCrate'
    idx = d.find(lit)
    print(path.split('\\')[-1], 'literal count', d.count(lit), 'first off', hex(idx))
    if idx < 0:
        return
    sva = idx + lo
    i = 0
    while True:
        i = d.find(b'\x68', i)
        if i < 0:
            print('  no push ref')
            return
        imm = struct.unpack_from('<I', d, i + 1)[0]
        if imm == sva:
            for k in range(i + 5, min(i + 15, len(d) - 5)):
                if d[k] == 0xE8:
                    rel = struct.unpack_from('<i', d, k + 1)[0]
                    print('  push@%08X call@%08X -> target %08X (rva %X)' % (
                        lo + i, lo + k, (lo + k + 5 + rel) & 0xFFFFFFFF,
                        ((lo + k + 5 + rel) & 0xFFFFFFFF) - lo))
                    return
        i += 1

discover(r'C:\Users\YTMartian\Desktop\py\redalert3\ra3_1.12.game')
discover(r'H:\steam client\steamapps\common\Command and Conquer Red Alert 3 Uprising\Data\ra3ep1_1.0.game')
