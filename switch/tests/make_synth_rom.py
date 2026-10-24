#!/usr/bin/env python3
"""Builds synthetic USA-header ROMs (z64, v64, n64 byte order) whose asset ranges hold PERS-SZP/Yay0 compressed copies of the
decoded files of a tools/dump_stadium_ui.py dump, so tests/skin_test.cpp can check the C++ decompressor and ROM reader.
usage: make_synth_rom.py <dump dir> <out dir>"""
import struct, sys, os

def yay0(data):
    n = len(data)
    masks, links, chunk = [], bytearray(), bytearray()
    ops = []   # ('lit', byte) or ('ref', dist, len)
    table = {}
    def find(i):
        best_len, best_dist = 0, 0
        if i + 3 <= n:
            for j in reversed(table.get(data[i:i+3], [])[-400:]):
                if i - j > 4096: break
                l = 3
                while l < 273 and i + l < n and data[j + l] == data[i + l]: l += 1
                if l > best_len: best_len, best_dist = l, i - j
                if l == 273: break
        return best_len, best_dist
    added = [0]
    def ensure(upto):   # hash every position below `upto` exactly once
        while added[0] < min(upto, n - 2):
            table.setdefault(data[added[0]:added[0] + 3], []).append(added[0]); added[0] += 1
        added[0] = max(added[0], min(upto, n))
    i = 0
    while i < n:
        ensure(i)
        best_len, best_dist = find(i)
        if best_len >= 3:                       # lazy matching: a clearly longer match one byte later wins
            ensure(i + 1)
            if find(i + 1)[0] > best_len + 1: best_len = 0
        if best_len >= 3:
            ops.append(('ref', best_dist, best_len)); i += best_len
        else:
            ops.append(('lit', data[i])); i += 1
    bits = 0; cnt = 0; words = []
    for op in ops:
        bits <<= 1
        if op[0] == 'lit':
            bits |= 1; chunk.append(op[1])
        else:
            _, d, l = op
            if l >= 18:
                links += struct.pack('>H', d - 1); chunk.append(l - 18)
            else:
                links += struct.pack('>H', ((l - 2) << 12) | (d - 1))
        cnt += 1
        if cnt == 32: words.append(bits); bits = 0; cnt = 0
    if cnt: words.append(bits << (32 - cnt))
    mask = b''.join(struct.pack('>I', w) for w in words)
    link_off = 16 + len(mask); chunk_off = link_off + len(links)
    return b'Yay0' + struct.pack('>III', n, link_off, chunk_off) + mask + bytes(links) + bytes(chunk)

def pers_szp(data):
    # Zero one word and restore it through a patch entry; ask for 16 more zero bytes than the stream holds.
    off = 0x100
    value = struct.unpack('>I', data[off:off+4])[0]
    stripped = data[:off] + b'\0\0\0\0' + data[off+4:]
    body = yay0(stripped)
    header = b'PERS-SZP' + struct.pack('>IIII', 0x18 + 8, len(body), len(data) + 16, 1) + struct.pack('>II', value, off)
    return header + body

def multi(files):
    head = struct.pack('>IIII', 0, 0, 0, len(files)); table = b''; blob = b''
    base = 0x10 + 16 * len(files)
    for f in files:
        table += struct.pack('>IIII', base + len(blob), len(f), 0, 0); blob += f
    total = base + len(blob)
    return struct.pack('>IIII', 0, 0, total, len(files)) + table + blob

dump, out = sys.argv[1], sys.argv[2]
rd = lambda p: open(os.path.join(dump, p), 'rb').read()
rom = bytearray(0x800000)
rom[0:4] = bytes([0x80, 0x37, 0x12, 0x40])
rom[0x10:0x18] = struct.pack('>II', 0x90F5D9B3, 0x9D0EDCF0)
def put(start, end, blob):
    assert len(blob) <= end - start, (hex(start), len(blob), end - start)
    rom[start:start+len(blob)] = blob
filler = pers_szp(bytes(range(256)) * 2)
put(0x3BA190, 0x3CB130, multi([filler, filler, filler, pers_szp(rd('fonts/3.bin'))]))
# Only the part the picker reads: this encoder is a little weaker than Nintendo's, and the full file would not fit the range.
put(0x4BE810, 0x4D1330, pers_szp(rd('common_menu2_ui/0.bin')[:0x28A00]))
put(0x6B4F00, 0x6C1AB0, pers_szp(rd('gb_pak_select_ui/0.bin')[:0x8800]))
put(0x4EB0C0, 0x5046B0, pers_szp(rd('menu_select_ui/0.bin')[:0x10800]))
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'synth.z64'), 'wb').write(rom)
v = bytearray(rom)
for i in range(0, len(v), 2): v[i], v[i+1] = v[i+1], v[i]
open(os.path.join(out, 'synth.v64'), 'wb').write(v)
n = bytearray(rom)
for i in range(0, len(n), 4): n[i:i+4] = n[i:i+4][::-1]
open(os.path.join(out, 'synth.n64'), 'wb').write(n)
bad = bytearray(rom); bad[0x10] ^= 1
open(os.path.join(out, 'bad.z64'), 'wb').write(bad)
