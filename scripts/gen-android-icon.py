#!/usr/bin/env python3
"""Generate the launcher icon set into psz/android/res/.

Original artwork on purpose: no game asset and no Sega mark is used or traced,
so nothing copyrighted ships in a public repo. The motif is a crescent planet
limb over a starfield with a light shaft — generic space iconography, chosen to
read as clearly NOT melonDS's melon at launcher size.
"""
import math, pathlib, random
from PIL import Image, ImageDraw, ImageFilter

OUT = pathlib.Path(__file__).resolve().parent.parent / "psz" / "android" / "res"
NAVY = (11, 27, 58)
CYAN = (120, 214, 255)
SIZES_LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
SIZES_FG     = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
S = 1024   # master, downsampled per density


def art(transparent):
    """The motif at master size. `transparent` omits the background plate."""
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if not transparent:
        d.rectangle([0, 0, S, S], fill=NAVY + (255,))
        rnd = random.Random(7)          # fixed seed: regenerating is deterministic
        for _ in range(220):
            x, y = rnd.randrange(S), rnd.randrange(S)
            r = rnd.choice([1, 1, 2, 3])
            a = rnd.randrange(60, 190)
            d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, a))

    # Light shaft: a soft vertical wedge through the centre.
    beam = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(S * 0.5 - S * 0.045, 0), (S * 0.5 + S * 0.045, 0),
                (S * 0.5 + S * 0.12, S), (S * 0.5 - S * 0.12, S)],
               fill=(190, 235, 255, 130))
    beam = beam.filter(ImageFilter.GaussianBlur(S * 0.035))
    im = Image.alpha_composite(im, beam)

    # Crescent: a bright ring with an offset disc punched out of it.
    ring = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    cx, cy, r = S * 0.5, S * 0.47, S * 0.30
    rd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=CYAN + (255,))
    hole = Image.new("L", (S, S), 0)
    hd = ImageDraw.Draw(hole)
    hr = r * 0.86
    hd.ellipse([cx - hr + r * 0.30, cy - hr - r * 0.12,
                cx + hr + r * 0.30, cy + hr - r * 0.12], fill=255)
    ring.putalpha(Image.composite(Image.new("L", (S, S), 0), ring.getchannel("A"), hole))
    glow = ring.filter(ImageFilter.GaussianBlur(S * 0.02))
    im = Image.alpha_composite(im, glow)
    return Image.alpha_composite(im, ring)


def write(img, path, size):
    path.parent.mkdir(parents=True, exist_ok=True)
    img.resize((size, size), Image.LANCZOS).save(path)


full, fg = art(False), art(True)
for dens, px in SIZES_LEGACY.items():
    write(full, OUT / f"mipmap-{dens}" / "ic_launcher.png", px)
    write(full, OUT / f"mipmap-{dens}" / "ic_launcher_round.png", px)
for dens, px in SIZES_FG.items():
    # Adaptive foregrounds are cropped to the inner ~66%, so shrink into that.
    pad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    inner = fg.resize((int(S * 0.66), int(S * 0.66)), Image.LANCZOS)
    pad.paste(inner, (int(S * 0.17), int(S * 0.17)), inner)
    write(pad, OUT / f"mipmap-{dens}" / "ic_launcher_foreground.png", px)

(OUT / "drawable").mkdir(parents=True, exist_ok=True)
(OUT / "drawable" / "psz_icon_background.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<shape xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <solid android:color="#0B1B3A"/>\n'
    '</shape>\n')
(OUT / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
(OUT / "mipmap-anydpi-v26" / "ic_launcher.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <background android:drawable="@drawable/psz_icon_background"/>\n'
    '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
    '</adaptive-icon>\n')
(OUT / "mipmap-anydpi-v26" / "ic_launcher_round.xml").write_text(
    (OUT / "mipmap-anydpi-v26" / "ic_launcher.xml").read_text())
print("wrote icon set to", OUT)
