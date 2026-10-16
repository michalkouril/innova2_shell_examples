#!/usr/bin/env python3
import sys, random, struct
sys.path.insert(0, '.')
from zuc_ref import ZUC, eia3_raw, eea3_iv
random.seed(7)
def ipcsum(b):
    s = sum(struct.unpack('!10H', b)); s = (s & 0xffff) + (s >> 16); s = (s & 0xffff) + (s >> 16); return (~s) & 0xffff
def l0(frame_len, pad=0xAA):
    eth = bytes.fromhex('020000000001' '020000000002' '0800')
    iplen = frame_len - 14
    ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, iplen, 0x1234, 0x4000, 64, 17, 0, bytes([10,0,0,1]), bytes([10,0,0,2])))
    ip[10:12] = struct.pack('!H', ipcsum(bytes(ip)))
    udp = struct.pack('!HHHH', 1234, 5678, frame_len - 34, 0xBEEF)
    b = eth + bytes(ip) + udp; return b + bytes([pad]) * (64 - len(b))
def hdr(op, length, msgid, key, iv):
    b = bytes([op, 0x5A]) + struct.pack('!H', length) + struct.pack('!Q', msgid) + key + iv
    return b + bytes([0xCC]) * (64 - len(b))
tests = []
def add(name, op, length, payload=None, key=None, iv=None, nlines=None, hlen=None):
    key = key or bytes(random.randrange(256) for _ in range(16)); iv = iv or bytes(random.randrange(256) for _ in range(16))
    payload = payload if payload is not None else bytes(random.randrange(256) for _ in range(length))
    tests.append(dict(name=name, op=op, len=length, payload=payload, key=key, iv=iv, nlines=nlines, hlen=hlen))
k1 = bytes.fromhex('173d14ba5003731d7a60049470f00a29')
p1 = bytes.fromhex('6cf65340735552ab0c9752fa6f9025fe0bd675d9005875b2')
add('EEA3-TS1 CONF 24B', 0, 24, p1, k1, bytes(eea3_iv(0x66035492, 0xf, 0)))
add('INTEG 13B', 1, 13); add('CONF 100B', 0, 100); add('INTEG 64B', 1, 64); add('CONF 64B', 0, 64)
add('INTEG 200B', 1, 200); add('CONF 1000B', 0, 1000); add('CONF 1B', 0, 1); add('INTEG 1B', 1, 1)
add('MISMATCH hdr300 1 line', 0, 300, nlines=1); add('OPCODE 4', 4, 64)
add('CONF 9000B', 0, 9000); add('CONF 8195B', 0, 8195); add('INTEG 8191B', 1, 8191); add('INTEG 8192B', 1, 8192)
add('CONF 2B after big', 0, 2)
def build():
    lines = []
    for n, t in enumerate(tests):
        pl = t['payload']; nl = t['nlines'] if t['nlines'] is not None else (len(pl) + 63) // 64
        body = pl + bytes([0xEE]) * (nl * 64 - len(pl)); body = body[:nl * 64]
        frame = l0(128 + len(pl)) + hdr(t['op'], t['len'], 0x1111000000000000 + n, t['key'], t['iv']) + body
        chid, ctx, afu = 3, 5, n % 16
        beats = [frame[i:i+64] for i in range(0, len(frame), 64)]
        for i, b in enumerate(beats):
            last = 1 if i == len(beats) - 1 else 0
            tuser = (afu << 64) | (ctx << 60) | (chid << 56) | (1 << 39) | (0 << 30) | ((len(frame) & 0x3fff) << 16)
            lines.append('%0128x %x %018x' % (int.from_bytes(b, 'big'), last, tuser))
    open('in.txt', 'w').write('\n'.join(lines) + '\n')
def check():
    pk, cur, csr = [], [], []
    for l in open('out.txt'):
        f = l.split()
        if f[0] == 'OUT' and ('x' in f[1] or 'x' in f[3]):
            xb = globals().get('XB', 0) + 1; globals()['XB'] = xb; continue
        if f[0] == 'OUT':
            cur.append((int(f[1], 16).to_bytes(64, 'big'), int(f[2], 16), int(f[3]), int(f[4], 16)))
            if f[3] == '1': pk.append(cur); cur = []
        else: csr.append(l.strip())
    print('packets out:', len(pk), 'tests:', len(tests), 'X-beats(uninitialised data out):', globals().get('XB', 0))
    for c in csr: print('  ', c)
    for p in pk:
        # identify by message id in header line
        keep_bytes = lambda k: bin(k).count('1')
        data = b''.join(b[:keep_bytes(k)] for b, k, _, _ in p)
        allb = b''.join(b for b, _, _, _ in p)
        msgid = int.from_bytes(allb[68:76], 'big'); n = msgid - 0x1111000000000000
        if not (0 <= n < len(tests)):
            print('?? unknown packet, %d beats, first bytes %s' % (len(p), allb[:16].hex())); continue
        t = tests[n]
        l0b = allb[:64]
        info = 'beats=%d bytes(keep)=%d tuser=%x keeps=%s' % (len(p), len(data), p[-1][3], ','.join('%016x' % k for _, k, _, _ in p[-2:]))
        eth_ok = l0b[0:6] == bytes.fromhex('020000000002') and l0b[6:12] == bytes.fromhex('020000000001')
        ipsw = l0b[26:30] == bytes([10,0,0,2]) and l0b[30:34] == bytes([10,0,0,1]); udpsw = l0b[34:36] == struct.pack('!H', 5678)
        iplen, ipck, udplen, udpck = struct.unpack('!H', l0b[16:18])[0], l0b[24:26].hex(), struct.unpack('!H', l0b[38:40])[0], l0b[40:42].hex()
        rh = data[64:128]
        if t['op'] == 0 and t['nlines'] is None:
            z = ZUC(list(t['key']), list(t['iv'])).words((t['len'] + 3) // 4)
            ks = b''.join(w.to_bytes(4, 'big') for w in z)[:t['len']]
            exp = bytes(a ^ b for a, b in zip(t['payload'], ks)); got = data[128:]
            res = 'CONF %s' % ('MATCH' if got == exp else 'MISMATCH first diff @%s' % next((i for i in range(min(len(got),len(exp))) if got[i]!=exp[i]), 'len'))
            if n == 0: res += ' ct=%s' % got.hex()
        elif t['op'] == 1:
            mac = eia3_raw(t['key'], t['iv'], t['payload'], 8 * t['len']); got = int.from_bytes(rh[44:48], 'big')
            res = 'INTEG mac got %08x exp %08x %s' % (got, mac, 'MATCH' if got == mac else 'MISMATCH')
        else:
            res = 'BYPASS-ish: frame echoed=%s' % (allb[:64 * (2 + (t['nlines'] if t['nlines'] is not None else (t['len']+63)//64))] ==
                  (l0(128 + len(t['payload']))[:42] + bytes(22)))
        print('%-24s %s | eth_swapped=%s ip_swapped=%s udp_swapped=%s iplen=%d ipck=%s udplen=%d udpck=%s l0[42:64]=%s | rh[0:16]=%s rh[16:48]=%s | %s' % (
            t['name'], res, eth_ok, ipsw, udpsw, iplen, ipck, udplen, udpck, l0b[42:64].hex(), rh[:16].hex(), rh[16:48].hex(), info))
if __name__ == '__main__':
    build() if sys.argv[1] == 'build' else check()
