#!/usr/bin/env python3
"""Dump the UI assets of your own Pokemon Stadium (USA) ROM so the Switch cartridge picker can be skinned like the game.

    python3 dump_stadium_ui.py "Pokemon Stadium (USA).z64" [out_dir] [--all]

Pure Python 3, no packages needed (Pillow is used for a background contact sheet if it is installed).
Writes out_dir/ (default ./stadium_ui_dump) and out_dir.zip. Nothing here is uploaded anywhere; the dump contains your
ROM's graphics, so it is for private use only. Offsets come from the pret/pokestadium decompilation (yamls/us/rom.yaml).
"""
import os
import struct
import sys
import zlib
import zipfile

# (rom offset, name) of the asset bins; a bin ends where the next one starts.
ASSETS = [
    (0x3B96B0, "n64_logo_texture"), (0x3BA190, "fonts"), (0x3CB130, "backgrounds"), (0x45B580, "battle_headers"),
    (0x4A21C0, "common_menu0_ui"), (0x4BD6B0, "common_menu1_ui"), (0x4BE810, "common_menu2_ui"),
    (0x4D1330, "area_select_ui"), (0x4EB0C0, "menu_select_ui"), (0x5046B0, "stadium_select_ui"), (0x52ABA0, "title_ui"),
    (0x52F6D0, "unused_52F6D0"), (0x533B20, "battle_portraits"), (0x56E7D0, "stadium_models"), (0x5C7A70, "unk_5C7A70"),
    (0x5C7AD0, "pokedex_area_model"), (0x5F6790, "sushi_go_round"), (0x5F92A0, "order_select_ui"),
    (0x5FFC30, "cup_ball_select_ui"), (0x613110, "trade_select_ui"), (0x6230D0, "kids_club_select_ui"),
    (0x66E1D0, "gym_leader_castle_clear_gfx"), (0x66F540, "rental_rules_ui"), (0x66FD80, "sushi_go_round_sprites"),
    (0x675FA0, "kids_club_game_ui"), (0x696350, "gallery_ui"), (0x69D090, "album_ui"), (0x6A9750, "print_ui"),
    (0x6AE1D0, "snap_select_ui"), (0x6B0FB0, "cup_clear_ui"), (0x6B4F00, "gb_pak_select_ui"),
    (0x6C1AB0, "victory_palace_plate_text"), (0x6C4CE0, "run_rattata_run_flag_gfx"), (0x6C4E50, "kanto_gb_map"),
    (0x6CA730, "type_backgrounds"), (0x6E2F90, "blank"), (0x6E2FC0, "battle_ui"), (0x6E8910, "transfer_pak_error_ui"),
    (0x6EB340, "pokedex_area_flags"), (0x6EC4D0, "gb_pak_unused_ui"), (0x6ECE80, "badge_ui"),
    (0x701E50, "gym_leader_castle_ui"), (0x70D3A0, "unk_70D3A0"), (0x7820E0, "unk_7820E0"), (0x783760, "textdata"),
    (0x798CD0, "unk_798CD0"), (0x7994B0, "snap_mode_ui"), (0x79AFB0, "copyright"), (0x79B8F0, None),
]
DEFAULT = ["fonts", "backgrounds", "gb_pak_select_ui", "gb_pak_unused_ui", "transfer_pak_error_ui", "common_menu0_ui",
           "common_menu1_ui", "common_menu2_ui", "menu_select_ui", "title_ui", "type_backgrounds"]
# Text engine tables of the GB Tower fragment (vram 0x8120E300.., ROM = 0x7F9A0 + vram - 0x81200000).
TABLES = (0x7F9A0 + 0xE300, 0x7F9A0 + 0xE700)


def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]


def yay0(data):
    if data[:4] != b"Yay0":
        raise ValueError("not Yay0")
    size, link, chunk = u32(data, 4), u32(data, 8), u32(data, 12)
    out, mask_pos, bits, left = bytearray(), 16, 0, 0
    while len(out) < size:
        if left == 0:
            bits, mask_pos, left = u32(data, mask_pos), mask_pos + 4, 32
        if bits & 0x80000000:
            out.append(data[chunk]); chunk += 1
        else:
            l = u16(data, link); link += 2
            dist, n = (l & 0xFFF) + 1, l >> 12
            if n == 0:
                n = data[chunk] + 18; chunk += 1
            else:
                n += 2
            for _ in range(n):
                out.append(out[-dist])
        bits = (bits << 1) & 0xFFFFFFFF
        left -= 1
    return bytes(out[:size])


def decode(blob):
    """-> (kind, data). PERS-SZP = 0x18-byte header (size at +8) then Yay0; PRESJPEG = 0x10 header then a JPEG."""
    if blob[:8] == b"PERS-SZP":
        # Header: magic[8], header_size, size1 (Yay0 output), size2 (size1 + zeroed bss), patch count, then `count` pairs
        # {value, offset}: the loader (func_80003890) writes `value` as a word at `offset` after decompressing.
        header_size, size1, size2, patches = struct.unpack_from(">IIII", blob, 8)
        data = bytearray(yay0(blob[header_size:]))
        data += b"\0" * max(0, size2 - len(data))
        for i in range(patches):
            value, offset = struct.unpack_from(">II", blob, 0x18 + i * 8)
            if offset + 4 <= len(data):
                struct.pack_into(">I", data, offset, value)
        return "PERS-SZP", bytes(data)
    if blob[:8] == b"PRESJPEG":
        return "PRESJPEG", blob[u32(blob, 8):]
    return "raw", blob


def split_multi(blob):
    """Multi-asset bin: 4 header words (size at +8, count at +12), then {offset, size, ?, ?} x count."""
    if len(blob) >= 16 and blob[0:3] == b"\0\0\0" and blob[4:8] == b"\0\0\0\0" and 0 < u32(blob, 12) < 4096:
        count = u32(blob, 12)
        files = []
        for i in range(count):
            off, size = u32(blob, 0x10 + i * 16), u32(blob, 0x14 + i * 16)
            if off + size > len(blob):
                return [blob]
            files.append(blob[off:off + size])
        return files
    return [blob]


# ---- tiny PNG writer ---------------------------------------------------------------------------------------------------
def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


DIGITS = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
          "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
          "8": "111101111101111", "9": "111101111001111", "A": "010101111101101", "B": "110101110101110",
          "C": "011100100100011", "D": "110101101101110", "E": "111100110100111", "F": "111100110100100"}


def font_sheet(path, data, rows, per_row=16, scale=2):
    """Glyph tiles of the 'fonts' asset: from +0x90, `rows` rows of 16 bytes per tile, low nibble of each byte = shade."""
    body = data[0x90:]
    count = len(body) // (rows * 16)
    cell_w, cell_h = 16 * scale + 4, rows * scale + 10
    cols, lines = per_row, (count + per_row - 1) // per_row
    w, h = cols * cell_w, lines * cell_h
    img = bytearray(b"\x20\x20\x30" * (w * h))
    for t in range(count):
        cx, cy = (t % cols) * cell_w, (t // cols) * cell_h
        for i, ch in enumerate("%02X" % t if t < 256 else "%03X" % t):
            for j, bit in enumerate(DIGITS[ch]):
                if bit == "1":
                    px, py = cx + 1 + i * 4 + j % 3, cy + 1 + j // 3
                    o = (py * w + px) * 3
                    img[o:o + 3] = b"\xff\xd0\x40"
        tile = body[t * rows * 16:(t + 1) * rows * 16]
        for y in range(rows):
            for x in range(16):
                v = (tile[y * 16 + x] & 0xF) * 17
                for dy in range(scale):
                    for dx in range(scale):
                        o = ((cy + 8 + y * scale + dy) * w + cx + x * scale + dx) * 3
                        img[o:o + 3] = bytes((v, v, v))
    write_png(path, w, h, img)
    return count


def rgb5551(v): return "#%02X%02X%02X" % (((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__); return 1
    rom = open(args[0], "rb").read()
    if rom[:4] == b"\x37\x80\x40\x12":     # .v64 (byte-swapped)
        rom = b"".join(rom[i + 1:i + 2] + rom[i:i + 1] for i in range(0, len(rom), 2))
    elif rom[:4] == b"\x40\x12\x37\x80":   # .n64 (little endian words)
        rom = b"".join(rom[i:i + 4][::-1] for i in range(0, len(rom), 4))
    if rom[:4] != b"\x80\x37\x12\x40":
        print("not an N64 ROM"); return 1
    out = args[1] if len(args) > 1 else "stadium_ui_dump"
    os.makedirs(out, exist_ok=True)
    report = ["title: %r  size: 0x%X  crc: %08X %08X" % (rom[0x20:0x34].decode("ascii", "replace"), len(rom), u32(rom, 0x10), u32(rom, 0x14))]
    wanted = None if "--all" in sys.argv else set(DEFAULT)

    for index, (start, name) in enumerate(ASSETS[:-1]):
        if wanted is not None and name not in wanted:
            continue
        blob = rom[start:ASSETS[index + 1][0]]
        files = split_multi(blob)
        report.append("== %s @0x%X size 0x%X: %d file(s)" % (name, start, len(blob), len(files)))
        for i, f in enumerate(files):
            try:
                kind, data = decode(f)
            except Exception as e:  # keep going: report the failure
                kind, data = "error:%s" % e, f
            ext = "jpg" if kind == "PRESJPEG" else "bin"
            d = os.path.join(out, name)
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, "%d.%s" % (i, ext)), "wb").write(data)
            report.append("  [%2d] %-9s stored 0x%-6X -> 0x%-6X  %s" % (i, kind, len(f), len(data), data[:16].hex()))
            if name == "fonts" and kind != "raw" or name == "fonts" and i < 2:
                rows = 10 if i == 0 else 12
                n = font_sheet(os.path.join(out, "font%d_sheet_%drows.png" % (i + 1, rows)), data, rows)
                report.append("       font sheet: %d tiles of 16x%d, header: %s" % (n, rows, data[:0x90].hex()))

    # Raw ROM ranges used by the transfer-pak screens (fragments 31-33): the Game Pak Check background is a PRESJPEG here.
    for lo, hi, label in ((0x2552B0, 0x261F70, "fragments31_33"), (0x25F440, 0x261F70, "fragment33_misc")):
        open(os.path.join(out, "rom_%X_%X_%s.bin" % (lo, hi, label)), "wb").write(rom[lo:hi])
    try:
        kind, data = decode(rom[0x25F440:0x261F70])
        if kind == "PRESJPEG":
            open(os.path.join(out, "gamepak_check_background.jpg"), "wb").write(data)
        report.append("fragment33_misc @0x25F440: %s, %d bytes, starts %s" % (kind, len(data), data[:8].hex()))
    except Exception as e:
        report.append("fragment33_misc: %s" % e)

    tables = rom[TABLES[0]:TABLES[1]]
    open(os.path.join(out, "fragment1_text_tables.bin"), "wb").write(tables)
    report.append("== GB Tower text tables (vram 0x8120E300..): 12 palettes of 16 x RGBA5551, then glyph maps")
    for p in range(12):
        report.append("  palette %2d: %s" % (p, " ".join(rgb5551(u16(tables, p * 32 + i * 2)) for i in range(16))))
    report.append("  map @E480 (0x180 bytes): " + tables[0x180:0x180 + 0x180].hex())
    report.append("  map @E580..: " + tables[0x280:0x280 + 0x180].hex())

    try:
        from PIL import Image  # optional: contact sheet of the background JPEGs
        d = os.path.join(out, "backgrounds")
        names = sorted((n for n in os.listdir(d) if n.endswith(".jpg")), key=lambda n: int(n.split(".")[0])) if os.path.isdir(d) else []
        ims = []
        for n in names:
            try:
                ims.append((n, Image.open(os.path.join(d, n)).convert("RGB")))
            except Exception:
                pass
        if ims:
            tw, th, cols = 160, 120, 6
            sheet = Image.new("RGB", (cols * tw, ((len(ims) + cols - 1) // cols) * th))
            for k, (n, im) in enumerate(ims):
                sheet.paste(im.resize((tw, th)), ((k % cols) * tw, (k // cols) * th))
            sheet.save(os.path.join(out, "backgrounds_sheet.png"))
            report.append("backgrounds_sheet.png: %d JPEGs in file order, %d per row, %dx%d each" % (len(ims), cols, tw, th))
    except ImportError:
        report.append("(Pillow not installed: no contact sheet; the .jpg files are in backgrounds/)")

    open(os.path.join(out, "report.txt"), "w").write("\n".join(report) + "\n")
    with zipfile.ZipFile(out.rstrip("/\\") + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, names in os.walk(out):
            for n in names:
                p = os.path.join(root, n)
                z.write(p, os.path.relpath(p, os.path.dirname(os.path.abspath(out))))
    print("\n".join(report[:40]))
    print("\nwrote %s and %s.zip" % (out, out.rstrip("/\\")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
