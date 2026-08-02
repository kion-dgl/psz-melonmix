#!/usr/bin/env python3
"""Emit psz/PSZArt.h -- HUD artwork and digit glyphs as compiled-in data.

WHY BAKE RATHER THAN LOAD

Loading PNGs at runtime needs a decoder and a per-platform asset path; on
Android the files have to live inside the APK and open through the asset
manager, which is a different code path from desktop's filesystem. Baking has
neither problem, and it matches how PSZOverlayIDs.h and PSZCheats.h already
work.

WHY RLE

Raw RGBA is too big for a header: map_grid.png alone is 1024x1004, which is
4 MB. HUD art is mostly flat colour and transparency, so run-length encoding
crushes it -- and the decoder is ten lines.

Regenerate:  python3 scripts/gen-art.py
"""
import pathlib
import sys

try:
    from PIL import Image, ImageFont, ImageDraw
except ImportError:
    sys.exit("!! needs Pillow: pip install pillow")

ROOT = pathlib.Path(__file__).resolve().parent.parent
ART = ROOT / "psz" / "art"
FONT = ROOT / "psz" / "font" / "Silkscreen-Bold.ttf"
OUT = ROOT / "psz" / "PSZArt.h"

# Only what is actually drawn. map_grid.png is deliberately absent: the area
# grid is not drawn from art yet, and baking a 1024x1004 image to feed a
# feature that does not exist is how a header becomes unreviewable.
IMAGES = [
    ("logo", "logo.png", 208),      # third field: resize width, for art drawn
    ("panel", "hp-pp.png"),         # much larger than its destination
    ("map", "map.png"),
    ("palette", "palette_bg.png"),
]

# BOLD, and bigger than the first pass. kion reported the HUD text as "really
# hard to read" on the Retroid: white Silkscreen Regular at 8px is a one-pixel
# stroke, and one pixel of DS resolution is a thin smear once the screen is
# scaled to a handheld. Bold doubles the stroke; the draw path adds an outline
# on top of that.
#
# Glyphs are alpha only, so one set tints to any colour.
# Full printable ASCII, not just digits: the info panel renders the game's own
# contextual text ("Vulkure", "18 Meseta", "Container") out of the UTF-16 buffer
# at 0x0211CCD0, so it needs letters, space and punctuation.
GLYPHS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyz"
          "0123456789 .,:;!?'\"()[]-+*/%#&@")
FONT_PX = 8


def rle(pixels):
    """(count, r, g, b, a) runs. Count is one byte, so runs cap at 255."""
    out = []
    run = None
    n = 0
    for p in pixels:
        if p == run and n < 255:
            n += 1
        else:
            if run is not None:
                out.append((n, *run))
            run, n = p, 1
    if run is not None:
        out.append((n, *run))
    return out


def emit_image(f, name, path, fitw=None):
    img = Image.open(path).convert("RGBA")
    if fitw and img.width > fitw:
        img = img.resize((fitw, max(1, img.height * fitw // img.width)), Image.LANCZOS)
    runs = rle(list(img.getdata()))
    raw = img.width * img.height * 4
    packed = len(runs) * 5
    f.write("// %s -- %dx%d, %d runs (%d -> %d bytes, %.0f%%)\n"
            % (path.name, img.width, img.height, len(runs), raw, packed,
               100.0 * packed / raw))
    f.write("static const unsigned char kArt_%s_rle[] = {\n" % name)
    flat = [str(v) for r in runs for v in r]
    for i in range(0, len(flat), 20):
        f.write("    " + ",".join(flat[i:i + 20]) + ",\n")
    f.write("};\n")
    f.write("static const PSZArtImage kArt_%s = { %d, %d, kArt_%s_rle, "
            "sizeof(kArt_%s_rle) };\n\n" % (name, img.width, img.height, name, name))
    return packed


def emit_glyphs(f):
    font = ImageFont.truetype(str(FONT), FONT_PX)
    # One measure pass: cells must be uniform so the draw side can index them
    # by character without a per-glyph table.
    boxes = [font.getbbox(c) for c in GLYPHS]
    top = min(b[1] for b in boxes)
    bot = max(b[3] for b in boxes)
    wid = max(b[2] for b in boxes)
    gh = bot - top
    gw = wid

    f.write("// Silkscreen %dpx, glyphs '%s' -- %dx%d cells, alpha only.\n"
            % (FONT_PX, GLYPHS, gw, gh))
    f.write("static const unsigned char kGlyphs[%d][%d] = {\n" % (len(GLYPHS), gw * gh))
    for c in GLYPHS:
        cell = Image.new("L", (gw, gh), 0)
        ImageDraw.Draw(cell).text((0, -top), c, font=font, fill=255)

        # THRESHOLD TO BINARY. Pillow antialiases TTF rendering, and the draw
        # path lights any non-zero pixel -- so a 12-alpha edge sample became a
        # fully lit white pixel. That is what put stray pixels around the top
        # of a "2", reading as a failed attempt at a curve instead of a glyph
        # on a grid. A pixel font has no partial coverage by design; storing it
        # with any is the bug.
        vals = [255 if v >= 128 else 0 for v in cell.getdata()]
        f.write("    { " + ",".join(str(v) for v in vals) + " },\n")
    f.write("};\n\n")
    f.write("static constexpr int kGlyphW = %d;\n" % gw)
    f.write("static constexpr int kGlyphH = %d;\n" % gh)
    esc = GLYPHS.replace("\\", "\\\\").replace('"', '\\"')
    f.write('static constexpr const char* kGlyphChars = "%s";\n\n' % esc)
    return len(GLYPHS) * gw * gh


def main():
    missing = [e[1] for e in IMAGES if not (ART / e[1]).is_file()]
    if missing:
        sys.exit("!! missing art: %s\n   run scripts/fetch-hud-art.sh first"
                 % ", ".join(missing))
    if not FONT.is_file():
        sys.exit("!! missing %s" % FONT)

    with OUT.open("w") as f:
        f.write("""// GENERATED by scripts/gen-art.py -- do not edit.
//
// HUD artwork and digit glyphs, compiled in. Images are run-length encoded as
// (count, r, g, b, a) 5-byte runs; PSZArtDecode expands one into RGBA.
//
// The glyphs are ours, not the game's: this overlay draws on top of melonDS's
// output, so the digits never had to come from the ROM. Silkscreen, OFL --
// see psz/font/OFL.txt.

#ifndef PSZART_H
#define PSZART_H

struct PSZArtImage
{
    int w, h;
    const unsigned char* rle;
    unsigned int rleLen;
};

""")
        total = 0
        for entry in IMAGES:
            name, fn = entry[0], entry[1]
            fitw = entry[2] if len(entry) > 2 else None
            total += emit_image(f, name, ART / fn, fitw)
        total += emit_glyphs(f)
        f.write("#endif\n")

    print(">> %s  (%.1f KB of data)" % (OUT, total / 1024.0))


if __name__ == "__main__":
    main()
