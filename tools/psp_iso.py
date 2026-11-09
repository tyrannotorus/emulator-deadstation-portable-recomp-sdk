#!/usr/bin/env python3
"""A PSP disc image's ISO9660 tree: its volume, the root and PSP_GAME listings, PARAM.SFO's keys, and one file extracted.
   psp_iso.py DISC.iso [PATH OUT]     e.g. psp_iso.py disc.iso PSP_GAME/SYSDIR/EBOOT.BIN EBOOT.BIN"""
import sys, struct
f = open(sys.argv[1], 'rb')
def sector(n, count=1):
    f.seek(n * 2048); return f.read(2048 * count)
pvd = sector(16)
assert pvd[1:6] == b'CD001', pvd[:8]
print('volume id:', pvd[40:72].decode('ascii', 'replace').strip(), '| size', struct.unpack('<I', pvd[80:84])[0], 'sectors')
def entries(lba, size):
    data = sector(lba, (size + 2047) // 2048)[:size]; i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048; continue
        rec = data[i:i + n]
        elba, esize, flags, nlen = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0], rec[25], rec[32]
        name = rec[33:33 + nlen]
        if name not in (b'\x00', b'\x01'):
            yield name.decode('ascii', 'replace').split(';')[0], elba, esize, bool(flags & 2)
        i += n
root = pvd[156:190]
rlba, rsize = struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0]
def find(path):
    lba, size = rlba, rsize
    for part in path.split('/'):
        for name, elba, esize, d in entries(lba, size):
            if name.upper() == part.upper():
                lba, size = elba, esize; break
        else:
            return None
    return lba, size
print('root:', [e[0] for e in entries(rlba, rsize)])
g = find('PSP_GAME'); print('PSP_GAME:', [ (e[0], e[2]) for e in entries(*g)] if g else None)
sys_dir = find('PSP_GAME/SYSDIR'); print('SYSDIR:', [(e[0], e[2]) for e in entries(*sys_dir)] if sys_dir else None)
sfo = find('PSP_GAME/PARAM.SFO')
if sfo:
    d = sector(sfo[0], (sfo[1] + 2047) // 2048)[:sfo[1]]
    keyt, datat, n = struct.unpack('<III', d[8:20])
    for k in range(n):
        ko, fmt, ln, mx, do = struct.unpack('<HHIII', d[20 + 16 * k: 36 + 16 * k])
        key = d[keyt + ko:d.index(b'\0', keyt + ko)].decode()
        val = d[datat + do:datat + do + ln]
        print(' ', key, '=', val.rstrip(b'\0').decode('utf-8', 'replace') if fmt != 0x404 else struct.unpack('<I', val)[0])
if len(sys.argv) > 3:
    loc = find(sys.argv[2])
    d = sector(loc[0], (loc[1] + 2047) // 2048)[:loc[1]]
    open(sys.argv[3], 'wb').write(d)
    print('extracted', sys.argv[2], len(d))
