import struct, sys, io

raw = open(r'C:\Users\YTMartian\Desktop\redalert3\RedAlert3_Trainer_1.12_FINAL3.exe', 'rb').read()
out = io.open(r'C:\Users\YTMartian\Desktop\redalert3\parsed_trainer.txt', 'w', encoding='utf-8')

def log(*a):
    out.write(' '.join(str(x) for x in a) + '\n')

data_off = struct.unpack_from('<I', raw, 80)[0]
log('trainer data offset: 0x%x' % data_off)

data = raw[data_off:]
pos = 0

def u32():
    global pos
    v = struct.unpack_from('<I', data, pos)[0]; pos += 4; return v
def s32():
    global pos
    v = struct.unpack_from('<i', data, pos)[0]; pos += 4; return v
def u8():
    global pos
    v = data[pos]; pos += 1; return v
def rawbytes(n):
    global pos
    v = data[pos:pos+n]; pos += n; return v

def read_string(decrypt=None, label=''):
    l = u32()
    if l > 200000:
        raise ValueError('bad string len %d at 0x%x (%s)' % (l, data_off+pos-4, label))
    b = bytearray(rawbytes(l))
    if decrypt is not None:
        for i in range(l):
            b[i] ^= (decrypt(i) & 0xff)
    return bytes(b)

def xor_key(base):
    return lambda i: (i + base) & 0xff

kind = u32(); version = u32(); count = u32()
log('kind=0x%x version=%d cheats=%d' % (kind, version, count))

cheats = []
try:
    for ci in range(count):
        desc = read_string(label='desc%d' % ci).decode('latin-1')
        hotkeytext = read_string(label='hk%d' % ci).decode('latin-1')
        hotkey = rawbytes(10)
        ncode = u32()
        codeentries = []
        for _ in range(ncode):
            addr = u32() ^ 0x11221122
            modname = read_string(label='mod%d' % ci).decode('latin-1')
            modoff = u32() ^ 0x22112211
            oplen = u32()
            opcode = rawbytes(oplen)
            codeentries.append(dict(address=addr, modulename=modname,
                                    moduleoffset=modoff, originalopcode=bytes(opcode)))
        naddr = u32()
        addressentries = []
        for _ in range(naddr):
            addr = u32() ^ 0xdead1337
            interp = read_string(decrypt=xor_key(2), label='interp%d' % ci).decode('latin-1')
            ispointer = u8()
            np = u32()
            pointers = []
            for _ in range(np):
                paddr = u32()
                pinterp = read_string(decrypt=xor_key(3), label='pinterp%d' % ci).decode('latin-1')
                off = s32()
                pointers.append(dict(address=paddr, interpretableaddress=pinterp, offset=off))
            bit = u8(); memtyp = u32(); frozen = u8(); frozendir = u8()
            setvalue = u8(); userinput = u8()
            value = read_string(label='val%d' % ci).decode('latin-1')
            script = read_string(decrypt=xor_key(1), label='script%d' % ci).decode('latin-1')
            addressentries.append(dict(
                address=addr, interpretableaddress=interp, ispointer=ispointer,
                pointers=pointers, bit=bit, memtyp=memtyp, frozen=frozen,
                frozendirection=frozendir, setvalue=setvalue, userinput=userinput,
                value=value, autoassemblescript=script))
        cheats.append(dict(description=desc, hotkeytext=hotkeytext, hotkey=list(hotkey),
                           codeentries=codeentries, addressentries=addressentries))
except Exception as e:
    log('PARSE ERROR:', repr(e))
    log('stopped at offset 0x%x' % (data_off + pos))

title = read_string(decrypt=xor_key(56)).decode('latin-1')
launchfile = read_string().decode('latin-1')
autolaunch = u8(); popup = u8()
process = read_string().decode('latin-1')
hktext2 = read_string().decode('latin-1')
hk2 = rawbytes(10)
aboutbox = read_string(decrypt=xor_key(166)).decode('latin-1')

log('--- trailer ---')
log('title: %r' % title)
log('launch file: %r' % launchfile)
log('autolaunch=%d popup=%d' % (autolaunch, popup))
log('process: %r' % process)
log('aboutbox: %r' % aboutbox[:300])
log('')
log('=== %d CHEATS ===' % len(cheats))
for i, c in enumerate(cheats):
    log('')
    log('[%d] desc=%r hotkey=%r' % (i, c['description'], c['hotkeytext']))
    for ce in c['codeentries']:
        log('   CODE: addr=0x%08X mod=%r modoff=0x%X op(%d)=%s' % (
            ce['address'], ce['modulename'], ce['moduleoffset'],
            len(ce['originalopcode']), ce['originalopcode'].hex()))
    for ae in c['addressentries']:
        log('   ADDR: addr=0x%08X interp=%r isptr=%s bit=%d memtyp=%d frozen=%s setval=%s uinput=%s value=%r' % (
            ae['address'], ae['interpretableaddress'], ae['ispointer'], ae['bit'],
            ae['memtyp'], ae['frozen'], ae['setvalue'], ae['userinput'], ae['value']))
        for p in ae['pointers']:
            log('      PTR: addr=0x%08X interp=%r off=%d' % (
                p['address'], p['interpretableaddress'], p['offset']))
        if ae['autoassemblescript']:
            log('      SCRIPT:')
            for ln in ae['autoassemblescript'].splitlines():
                log('         | ' + ln)

out.close()
print('done, wrote parsed_trainer.txt')
