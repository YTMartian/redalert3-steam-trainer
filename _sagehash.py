M = 0xFFFFFFFF


def t(c):
    # 引擎里 [0xbc74b8] 是个函数指针，调用点都是 tolower 风格。
    return ord(chr(c).lower()) & 0xFF


def sagehash(s, seed=None, finalize=True):
    b = s.encode('ascii')
    n = len(b)
    if seed is None:
        seed = n
    esi = seed & M
    rem = n & 3
    blocks = n >> 2
    for i in range(blocks):
        o = i * 4
        esi = (esi + (t(b[o + 1]) << 8 | t(b[o]))) & M
        tmp = ((t(b[o + 3]) << 8 | t(b[o + 2])) ^ ((esi << 5) & M)) & M
        esi = (esi ^ ((tmp << 11) & M)) & M
        esi = (esi + (esi >> 11)) & M
    o = blocks * 4
    if rem == 1:
        esi = (esi + t(b[o])) & M
        esi = (esi ^ ((esi << 10) & M)) & M
        esi = (esi + (esi >> 1)) & M
    elif rem == 2:
        esi = (esi + (t(b[o + 1]) << 8 | t(b[o]))) & M
        esi = (esi ^ ((esi << 11) & M)) & M
        esi = (esi + (esi >> 17)) & M
    elif rem == 3:
        esi = (esi + (t(b[o + 1]) << 8 | t(b[o]))) & M
        tmp = (((t(b[o + 2]) * 4) & M) ^ esi) & M
        esi = (esi ^ ((tmp << 16) & M)) & M
        esi = (esi + (esi >> 11)) & M
    if finalize:
        esi = (esi ^ ((esi * 8) & M)) & M
        esi = (esi + (esi >> 5)) & M
        esi = (esi ^ ((esi * 4) & M)) & M
        esi = (esi + (esi >> 15)) & M
        esi = (esi ^ ((esi << 10) & M)) & M
    return esi & M


tests = {
    'AttributeModifier_IronCurtain': 0x6516AA2D,
    'MAP_INVULNERABLE_Up': 0x508D8641,
    'MAP_SPEED_Up': 0x5949C29C,
    'MAP_SPEED_Down': 0x46C6A82B,
}
for k, v in tests.items():
    got = sagehash(k)
    print('%-32s want %08X got %08X %s' % (k, v, got, 'OK' if got == v else ''))
print()
for name in ['AttributeModifier_InvulnerabilityCrate', 'AttributeModifier_IronCurtain',
             'AttributeModifier_Invulnerability', 'AttributeModifier_Invulnerable',
             'MAP_INVULNERABLE_Up', 'AttributeModifier_SuperweaponIronCurtain']:
    print('%-40s %08X' % (name, sagehash(name)))
