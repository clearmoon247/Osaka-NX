#!/usr/bin/env python3
"""Tests switch/tools/rom_tool.py with made-up files (no real ROM is needed or used).

Why this file exists
    rom_tool.py decides whether a file is "the right game" by its MD5, so the test makes a tiny stand-in: it temporarily
    swaps the expected size and checksum for those of a small random file, then checks that all three byte orders are
    recognised, converted back to the same bytes, and that wrong files are rejected with a clear message.

Run:  python3 -I switch/tests/rom_tool_test.py      (prints "ROM TOOL OK" when everything passes)
"""
import hashlib
import os
import random
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import rom_tool  # noqa: E402


def check(condition, what):
    if not condition:
        print("FAIL:", what)
        sys.exit(1)


def main():
    rng = random.Random(1)
    size = 4 * 1024 * 1024 + 8                                   # not a multiple of the 1 MiB read size: tests the last chunk
    body = bytes(rng.getrandbits(8) for _ in range(size - 4))
    z64 = bytes.fromhex("80371240") + body

    def to_v64(data):                                            # swap every pair
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)

    def to_n64(data):                                            # reverse every group of four
        out = bytearray(data)
        out[0::4], out[1::4], out[2::4], out[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(out)

    # Make the tool believe this small file is the right game.
    rom_tool.EXPECTED_SIZE = size
    rom_tool.EXPECTED_MD5 = hashlib.md5(z64).hexdigest()

    with tempfile.TemporaryDirectory() as tmp:
        def write(name, data):
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(data)
            return path

        for name, data, order in (("a.z64", z64, "z64"), ("b.v64", to_v64(z64), "v64"), ("c.n64", to_n64(z64), "n64")):
            path = write(name, data)
            check(rom_tool.byte_order(path) == order, "byte order of " + name)
            check(rom_tool.inspect(path)[0], "right game accepted in " + order)
            out = os.path.join(tmp, "out_" + name + ".z64")
            check(rom_tool.convert(path, out)[0], "convert " + order)
            with open(out, "rb") as f:
                check(f.read() == z64, "converted " + order + " is identical to the z64 original")

        # A one-byte difference must be rejected, and the message must say it is the checksum.
        wrong = bytearray(z64)
        wrong[1000] ^= 1
        ok, message = rom_tool.inspect(write("wrong.z64", bytes(wrong)))
        check(not ok and "md5" in message, "changed byte rejected with an md5 message")

        # Wrong size, not a ROM at all, and a missing file.
        check(not rom_tool.inspect(write("short.z64", z64[:1024]))[0], "wrong size rejected")
        check("not an N64 ROM" in rom_tool.inspect(write("text.z64", b"hello world" * 1000))[1], "non-ROM rejected")
        check(not rom_tool.inspect(os.path.join(tmp, "missing.z64"))[0], "missing file rejected")

        # convert must refuse a wrong file and must not leave an output file behind.
        refused = os.path.join(tmp, "refused.z64")
        check(not rom_tool.convert(os.path.join(tmp, "wrong.z64"), refused)[0] and not os.path.exists(refused),
              "convert refuses a wrong file and writes nothing")

        for leftover in [n for n in os.listdir(tmp) if n.startswith("out_")]:     # the converted copies are valid ROMs too
            os.remove(os.path.join(tmp, leftover))

        # find: looks into sub-folders and ignores hidden ones; a folder with only the wrong file finds nothing.
        os.makedirs(os.path.join(tmp, "games", "n64"))
        os.makedirs(os.path.join(tmp, ".hidden"))
        os.replace(os.path.join(tmp, "b.v64"), os.path.join(tmp, "games", "n64", "stadium.v64"))
        os.replace(os.path.join(tmp, "a.z64"), os.path.join(tmp, ".hidden", "a.z64"))
        os.remove(os.path.join(tmp, "c.n64"))                                    # also a valid ROM; keep only the sub-folder one
        found = rom_tool.find([tmp])
        check(found == os.path.join(tmp, "games", "n64", "stadium.v64"), "find returns the match in a sub-folder: %s" % found)
        os.remove(found)
        check(rom_tool.find([tmp]) is None, "find returns None when only wrong files or hidden folders remain")

    print("ROM TOOL OK")


if __name__ == "__main__":
    main()
