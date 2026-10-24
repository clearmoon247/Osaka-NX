#!/usr/bin/env python3
"""Symbolizes the stall sampler ("prof") output of osaka-nx.log.
  switch/symbolize.sh /path/to/osaka-nx.log [stalled_ms]
Prints, for the sample block closest to stalled_ms (default: the longest stall), every thread's top PCs and the
caller (LR) as function names. Needs switch/build/osaka-nx.elf and devkitPro's aarch64 binutils."""
import os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
elf = os.path.join(here, "build", "osaka-nx.elf")
bindir = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin")
log = open(sys.argv[1], errors="replace").read().splitlines()
want = int(sys.argv[2]) if len(sys.argv) > 2 else None

marker_rt = None
for line in log:
    m = re.search(r"prof: marker \w+=0x([0-9a-f]+)", line)
    if m:
        marker_rt = int(m.group(1), 16)
        break
nm = subprocess.run([os.path.join(bindir, "aarch64-none-elf-nm"), elf], capture_output=True, text=True).stdout
# the logged marker is the address of psrnx_place_thread (psrnx_code_marker returns it)
marker_elf = next((int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == "psrnx_place_thread"), None)
if marker_rt is None or marker_elf is None:
    sys.exit("marker not found (log or ELF)")
base = marker_rt - marker_elf
print(f"load base 0x{base:x} (page aligned: {base % 0x1000 == 0})")

def sym(addr):
    out = subprocess.run([os.path.join(bindir, "aarch64-none-elf-addr2line"), "-f", "-C", "-e", elf, hex(addr - base)],
                         capture_output=True, text=True).stdout.splitlines()
    return out[0] if out else "?"

cache = {}
def name(addr):
    if addr not in cache:
        cache[addr] = sym(addr) if base <= addr < base + (1 << 30) else "(not ours: 0x%x)" % addr
    return cache[addr]

blocks, cur = [], None
for line in log:
    m = re.search(r"prof (mid|end): stalled (\d+) ms", line)
    if m:
        cur = {"ms": int(m.group(2)), "threads": []}
        blocks.append(cur)
        continue
    m = re.search(r"prof {3}(.{1,14}?) +n=(\d+) distinct=(\d+):(.*)", line)
    if m and cur is not None:
        ents = re.findall(r"pc=0x([0-9a-f]+)\(lr=0x([0-9a-f]+)\)x(\d+)", m.group(4))
        cur["threads"].append((m.group(1).strip(), int(m.group(2)), [(int(a, 16), int(b, 16), int(c)) for a, b, c in ents]))
if not blocks:
    sys.exit("no prof blocks in the log")
if want is None:
    best = max(blocks, key=lambda b: b["ms"])
else:
    best = min(blocks, key=lambda b: abs(b["ms"] - want))
print(f"block: stalled {best['ms']} ms")
for tname, n, ents in best["threads"]:
    print(f"\n{tname}  ({n} samples)")
    for pc, lr, c in ents:
        print(f"  x{c:<4} {name(pc)}\n         called from {name(lr)}")
