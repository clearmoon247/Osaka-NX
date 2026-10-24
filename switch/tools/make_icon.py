#!/usr/bin/env python3
"""Draws the Osaka-NX application icon (switch/icon.jpg) from scratch.

Why this file exists
    The icon is original artwork made only of geometric shapes drawn by this script: no photograph, no traced or
    imported picture, no font and no letters. Keeping the drawing as a short program means anyone can read exactly what is
    in the picture, and regenerate or change it.

    The picture shows a stylised Japanese castle keep (a nod to the name) in front of a golden sun, above a moat. It does not
    use any character, logo, colour scheme or shape from a game or a console maker.

Output
    switch/icon.jpg: 256 x 256, baseline (not progressive) JPEG, which is what the Switch homebrew packaging step (nacptool /
    elf2nro, via CMake's nx_create_nro ICON option) expects.

Use
    python3 switch/tools/make_icon.py            # rewrites switch/icon.jpg
    python3 switch/tools/make_icon.py out.png    # writes a PNG preview instead (any extension Pillow knows)

Needs Pillow (pip install pillow). Drawing is done at 4x size and scaled down, which smooths the edges.
"""
import os
import sys

from PIL import Image, ImageDraw

# ---- Size ---------------------------------------------------------------------------------------------------------------
FINAL = 256          # the icon's size in pixels (fixed by the Switch homebrew format)
SCALE = 4            # draw this many times larger, then shrink: smooth edges without a vector library
S = FINAL * SCALE    # size of the working canvas


def px(v):
    """Converts a coordinate written for a 256-wide picture into working-canvas pixels."""
    return int(round(v * SCALE))


def rgb(hex_string):
    """'#rrggbb' -> (r, g, b)."""
    h = hex_string.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# ---- Palette ------------------------------------------------------------------------------------------------------------
# Deep indigo night sky fading to teal, a warm gold sun, cream walls, dark slate roofs and stone. Chosen to be calm and
# readable at 64 px (hbmenu shows icons small), and not to match any console's or game's brand colours.
SKY_TOP = rgb("#1a2347")
SKY_BOTTOM = rgb("#1f7a7a")
SUN = rgb("#f4b940")
SUN_GLOW = rgb("#f7d77a")
WALL = rgb("#f5ecd5")
ROOF = rgb("#16283f")
ROOF_EDGE = rgb("#f4b940")
STONE = rgb("#2b3a55")
STONE_LINE = rgb("#1b2740")
WATER = rgb("#12304a")
WATER_LINE = rgb("#2f8f93")


# ---- Pieces of the picture -------------------------------------------------------------------------------------------------
def draw_sky(img):
    """Vertical gradient from SKY_TOP to SKY_BOTTOM over the whole canvas."""
    d = ImageDraw.Draw(img)
    for y in range(S):
        t = y / (S - 1)
        colour = tuple(int(SKY_TOP[i] + (SKY_BOTTOM[i] - SKY_TOP[i]) * t) for i in range(3))
        d.line([(0, y), (S, y)], fill=colour)


def draw_sun(img):
    """A gold disc with a paler ring around it, centred behind the keep."""
    d = ImageDraw.Draw(img)
    cx, cy = 128, 112
    for radius, colour in ((96, SUN_GLOW), (86, SUN)):   # outer ring first, then the disc over it
        d.ellipse([px(cx - radius), px(cy - radius), px(cx + radius), px(cy + radius)], fill=colour)


def draw_moat(img):
    """Dark water along the bottom with three thin lighter ripples."""
    d = ImageDraw.Draw(img)
    top = 196
    d.rectangle([0, px(top), S, S], fill=WATER)
    for i, y in enumerate((top + 14, top + 30, top + 46)):
        half = 70 - i * 18                                 # each ripple is a little shorter than the one above
        d.rounded_rectangle([px(128 - half), px(y), px(128 + half), px(y + 3)], radius=px(1.5), fill=WATER_LINE)


def draw_stone_base(img):
    """The sloping stone wall the keep stands on: a trapezoid with a few horizontal course lines."""
    d = ImageDraw.Draw(img)
    top, bottom = 150, 196
    d.polygon([(px(66), px(top)), (px(190), px(top)), (px(206), px(bottom)), (px(50), px(bottom))], fill=STONE)
    for y in (top + 15, top + 30):
        # The wall widens towards the bottom, so the line at height y runs between these two x values.
        t = (y - top) / (bottom - top)
        left, right = 66 - 16 * t, 190 + 16 * t
        d.line([(px(left), px(y)), (px(right), px(y))], fill=STONE_LINE, width=px(1.5))


def draw_tier(img, centre_x, wall_width, wall_bottom, wall_height, roof_height, eave):
    """One storey of the keep: a cream wall with a dark roof over it.

    centre_x      horizontal middle of the storey
    wall_width    width of the cream wall
    wall_bottom   y of the bottom edge of the wall
    wall_height   height of the wall
    roof_height   height of the roof above the wall
    eave          how far the roof overhangs the wall on each side
    """
    d = ImageDraw.Draw(img)
    left, right = centre_x - wall_width / 2, centre_x + wall_width / 2
    wall_top = wall_bottom - wall_height
    d.rectangle([px(left), px(wall_top), px(right), px(wall_bottom)], fill=WALL)

    # A single window in the middle of the wall.
    wx, ww = centre_x, wall_width * 0.16
    d.rectangle([px(wx - ww / 2), px(wall_top + wall_height * 0.28), px(wx + ww / 2), px(wall_top + wall_height * 0.78)], fill=ROOF)

    # The roof: a trapezoid wider than the wall, with the two lower corners lifted slightly (the upturned eaves of this kind
    # of building). Points run along the bottom edge first, then up the right side, along the top and down the left side.
    roof_bottom = wall_top
    roof_top = wall_top - roof_height
    lift = roof_height * 0.28
    inset = wall_width * 0.20
    roof = [
        (left - eave, roof_bottom - lift),        # left eave tip, lifted
        (left - eave * 0.55, roof_bottom + 1.2),  # left, just above the wall's top corner
        (right + eave * 0.55, roof_bottom + 1.2),
        (right + eave, roof_bottom - lift),        # right eave tip, lifted
        (right - inset, roof_top),                 # top right
        (left + inset, roof_top),                  # top left
    ]
    d.polygon([(px(x), px(y)) for x, y in roof], fill=ROOF)
    # Thin gold line along the lower edge of the roof, so the tiers read apart at small sizes.
    d.line([(px(left - eave * 0.55), px(roof_bottom + 1.2)), (px(right + eave * 0.55), px(roof_bottom + 1.2))],
           fill=ROOF_EDGE, width=px(1.6))


def draw_keep(img):
    """Three storeys, from the widest at the bottom to the narrowest at the top, then a small finial."""
    draw_tier(img, 128, wall_width=96, wall_bottom=150, wall_height=24, roof_height=22, eave=16)
    draw_tier(img, 128, wall_width=68, wall_bottom=104, wall_height=20, roof_height=19, eave=13)
    draw_tier(img, 128, wall_width=42, wall_bottom=65, wall_height=16, roof_height=17, eave=10)
    # Finial: a short dark spike at the very top (dark, because gold would vanish against the gold sun behind it).
    d = ImageDraw.Draw(img)
    d.polygon([(px(128), px(18)), (px(125), px(33)), (px(131), px(33))], fill=ROOF)


# ---- Putting it together ---------------------------------------------------------------------------------------------------
def build():
    """Draws every piece in back-to-front order and returns the finished 256 x 256 image."""
    img = Image.new("RGB", (S, S))
    draw_sky(img)       # 1. background
    draw_sun(img)       # 2. sun behind the building
    draw_moat(img)      # 3. water along the bottom
    draw_stone_base(img)  # 4. stone wall the keep stands on
    draw_keep(img)      # 5. the three storeys and the finial
    return img.resize((FINAL, FINAL), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "icon.jpg")
    image = build()
    if out.lower().endswith((".jpg", ".jpeg")):
        # progressive=False -> baseline JPEG; subsampling=0 keeps colour at full resolution (small flat-colour shapes stay crisp).
        image.save(out, "JPEG", quality=92, progressive=False, optimize=False, subsampling=0)
    else:
        image.save(out)
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
