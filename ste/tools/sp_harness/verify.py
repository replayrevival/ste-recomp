import importlib.util, math, os, struct, sys
spec = importlib.util.spec_from_file_location('sp', r'tools\mods\sound_pack.py'); sp = importlib.util.module_from_spec(spec); spec.loader.exec_module(sp)
C = sys.argv[1]
G = r'the development tree\game-owned\xbox\STE\extracted\SoundData'

def peak_freq(x, rate):
    best = (0, 0); n = min(len(x), 3000)
    for f in range(100, 3000, 10):
        re = sum(x[i] * math.cos(2 * math.pi * f * i / rate) for i in range(n))
        im = sum(x[i] * math.sin(2 * math.pi * f * i / rate) for i in range(n))
        m = re * re + im * im
        if m > best[0]: best = (m, f)
    return best[1]

for bank, slot, want in (('shell_sound', 1, 880), ('godzilla_voice_e', 28, 440)):
    u = open(os.path.join(C, bank + '.uber'), 'rb').read(); s = open(os.path.join(C, bank + '.samp'), 'rb').read()
    sl = {x['slot']: x for x in sp.parse_bank(u, s)}
    x = sl[slot]; ch = sp.adpcm_decode(x['data'], 1)[0]
    print(bank, slot, 'rate', x['rate'], 'frames', len(ch), 'secs %.2f' % (len(ch) / x['rate']), 'peak', max(ch), 'dominant freq', peak_freq(ch[200:], x['rate']), 'expected', want, 'loop', x['loop'])
    u0 = open(os.path.join(G, bank + '.uber'), 'rb').read(); s0 = open(os.path.join(G, bank + '.samp'), 'rb').read()
    o = {y['slot']: y for y in sp.parse_bank(u0, s0)}
    same = sum(1 for k in o if k != slot and k in sl and sl[k]['data'] == o[k]['data'] and sl[k]['rate'] == o[k]['rate'] and sl[k]['loop'] == o[k]['loop'])
    print('   other slots identical:', same, 'of', len(o) - 1)
    sd = struct.unpack_from('<I', u, 16)[0]
    diff = [i for i in range(len(u)) if u[i] != u0[i]]
    print('   uber bytes changed', len(diff), 'all inside SDIR', all(sd <= i < sd + 16 + (len(sl) + 1) * 16 for i in diff))
d = open(os.path.join(C, 'fight1.wxv'), 'rb').read(); ch, rate = sp.wxv_decode(d)
print('fight1', rate, 'frames', len(ch[0]), 'secs %.1f' % (len(ch[0]) / rate), 'peak', max(ch[0]), 'L freq', peak_freq(ch[0][100:], rate))
o = open(os.path.join(G, 'fight1.wxv'), 'rb').read(0x30)
print('orig hdr', o.hex()); print('new  hdr', d[:0x30].hex())
