#!/usr/bin/env python3
"""Finds, identifies, converts and verifies your Pokemon Stadium (USA) v1.0 ROM for the Osaka-NX build scripts.

Why this file exists
    The build (switch/codegen.sh) needs the ROM in "z64" byte order with one exact checksum. ROM dumps come in three byte orders
    (.z64, .v64, .n64) and with arbitrary names, so people often have the right game in the wrong format. This helper lets
    switch/build-nro.sh accept any of them and say clearly what is wrong when the file is not the right game.

    It only reads the file you point it at (and writes a converted copy when you ask for one). It never uploads or stores
    anything, and it has no dependencies beyond Python 3's standard library.

Commands (all print one line you can read; the exit status says whether it worked)
    rom_tool.py check   <file>             what byte order is it, and is it the right game?  exit 0 = right game
    rom_tool.py convert <file> <out.z64>   write a z64-order copy (works for any of the three orders); exit 0 = done
    rom_tool.py find    <folder> [...]     look inside these folders (3 levels deep) for the right game; prints the first match

Byte orders (every N64 ROM starts with the same 4-byte header word 0x80371240 in z64 order)
    z64  80 37 12 40   big-endian: the order the console uses and the build expects
    v64  37 80 40 12   every pair of bytes swapped
    n64  40 12 37 80   every group of four bytes reversed (little-endian words)
"""
import hashlib
import os
import sys

# ---- What the right game looks like ------------------------------------------------------------------------------------------
# Pokemon Stadium (USA) v1.0, in z64 order. Revision A / v1.1 has a different checksum and will not work with this project.
EXPECTED_MD5 = "ed1378bc12115f71209a77844965ba50"
EXPECTED_SIZE = 32 * 1024 * 1024          # 32 MiB; used to skip files that cannot be it without reading them
HEADER_WORDS = {                           # first four bytes -> byte order name
    bytes.fromhex("80371240"): "z64",
    bytes.fromhex("37804012"): "v64",
    bytes.fromhex("40123780"): "n64",
}
ROM_EXTENSIONS = (".z64", ".v64", ".n64", ".rom", ".bin")
CHUNK = 1024 * 1024                        # read files a megabyte at a time (the ROM is 32 MiB)


def byte_order(path):
    """Returns 'z64', 'v64' or 'n64' from the first four bytes of the file, or None if it is not an N64 ROM."""
    with open(path, "rb") as f:
        return HEADER_WORDS.get(f.read(4))


def to_z64_chunks(path, order):
    """Yields the file's contents converted to z64 byte order, one chunk at a time.

    CHUNK is a multiple of 4, so pairs and words never straddle two chunks.
    """
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                return
            if order == "v64":                                   # swap each pair: (a, b) -> (b, a)
                swapped = bytearray(len(chunk))
                swapped[0::2], swapped[1::2] = chunk[1::2], chunk[0::2]
                yield bytes(swapped)
            elif order == "n64":                                 # reverse each group of four
                swapped = bytearray(len(chunk))
                swapped[0::4], swapped[1::4], swapped[2::4], swapped[3::4] = chunk[3::4], chunk[2::4], chunk[1::4], chunk[0::4]
                yield bytes(swapped)
            else:
                yield chunk


def z64_md5(path, order):
    """MD5 of the file as it would be in z64 order."""
    digest = hashlib.md5()
    for chunk in to_z64_chunks(path, order):
        digest.update(chunk)
    return digest.hexdigest()


def inspect(path):
    """Returns (ok, message). ok is True when the file is the right game, in any byte order."""
    if not os.path.isfile(path):
        return False, "not found: %s" % path
    size = os.path.getsize(path)
    order = byte_order(path)
    if order is None:
        return False, "%s is not an N64 ROM (unknown header; is it zipped or renamed?)" % path
    if size != EXPECTED_SIZE:
        return False, "%s is an N64 ROM in %s order but %d bytes, not %d: a different game or an overdump/trimmed file" % (path, order, size, EXPECTED_SIZE)
    md5 = z64_md5(path, order)
    if md5 != EXPECTED_MD5:
        return False, ("%s is an N64 ROM (%s order) but not Pokemon Stadium (USA) v1.0: md5 %s, expected %s "
                       "(Revision A / v1.1 and other regions do not work)" % (path, order, md5, EXPECTED_MD5))
    return True, "%s is Pokemon Stadium (USA) v1.0 (%s byte order)" % (path, order)


def convert(path, out_path):
    """Writes a z64-order copy of the ROM. Returns (ok, message). Refuses anything that is not the right game."""
    ok, message = inspect(path)
    if not ok:
        return False, message
    order = byte_order(path)
    with open(out_path, "wb") as out:
        for chunk in to_z64_chunks(path, order):
            out.write(chunk)
    return True, "wrote %s (z64 order, converted from %s)" % (out_path, order)


def candidate_files(folders, max_depth=3):
    """Yields files in the given folders (up to max_depth levels down) that could be the ROM, judged by extension or exact size.

    Folders that cannot be read are skipped silently; hidden and system-looking folders are not entered.
    """
    skip = {"node_modules", "proc", "sys", "dev", "windows", "program files", "program files (x86)", "appdata", "$recycle.bin"}
    for top in folders:
        top = os.path.expanduser(top)
        if not os.path.isdir(top):
            continue
        top_depth = top.rstrip(os.sep).count(os.sep)
        for root, dirs, files in os.walk(top):
            if root.rstrip(os.sep).count(os.sep) - top_depth >= max_depth:
                dirs[:] = []                                       # do not go deeper
            dirs[:] = [d for d in dirs if not d.startswith(".") and d.lower() not in skip]
            for name in files:
                path = os.path.join(root, name)
                try:
                    if name.lower().endswith(ROM_EXTENSIONS) or os.path.getsize(path) == EXPECTED_SIZE:
                        yield path
                except OSError:
                    continue


def find(folders):
    """Returns the path of the first file in `folders` that is the right game, or None."""
    for path in candidate_files(folders):
        try:
            if os.path.getsize(path) == EXPECTED_SIZE and inspect(path)[0]:
                return path
        except OSError:
            continue
    return None


def main(argv):
    if len(argv) >= 3 and argv[1] == "check":
        ok, message = inspect(argv[2])
        print(message)
        return 0 if ok else 1
    if len(argv) == 4 and argv[1] == "convert":
        ok, message = convert(argv[2], argv[3])
        print(message)
        return 0 if ok else 1
    if len(argv) >= 3 and argv[1] == "find":
        found = find(argv[2:])
        if found:
            print(found)
            return 0
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
