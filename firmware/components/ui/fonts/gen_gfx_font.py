#!/usr/bin/env python3
"""Generate the terminal's proportional font tables (Adafruit-GFX format).

The type ramp STARTED as the metrics of the shipped product on this panel
(FreeSans 9pt body / 12pt bold emphasis / 18pt bold hero, rendered by
Adafruit fontconvert conventions: pt at 141 dpi, monochrome). Four bench
rounds of "bigger" (C7 rounds 3-6) escalated the reading sizes past that
reference; the fourth escalation (round 6) pushed body to a 17 px cap and
demoted the original 9pt body to a "small" chrome role (status strip,
MAC line). FreeSans itself is GPL, so the tables are generated fresh from
Liberation Sans (SIL OFL 1.1), whose Helvetica-compatible metrics track
the same ramp:

    role      face                        pt     cap  asc  desc
    small     LiberationSans-Regular      9.0     12   14    4
    body      LiberationSans-Regular     12.0     17   18    5
    emphasis  LiberationSans-Bold        15.0     20   22    6
    hero      LiberationSans-Bold        18.5     25   27    8

Provenance (pinned; the script refuses mismatched inputs):
    upstream  https://github.com/liberationfonts/liberation-fonts
    release   2.1.5 (2021-09-30)
    tarball   liberation-fonts-ttf-2.1.5.tar.gz
              https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
              sha256 7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0
    license   SIL Open Font License 1.1 (tarball LICENSE, vendored here as
              LICENSE.liberation)

Regeneration (from this directory):
    python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install freetype-py
    curl -L -o /tmp/liberation.tar.gz \
        https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
    tar -xzf /tmp/liberation.tar.gz -C /tmp
    /tmp/fontenv/bin/python gen_gfx_font.py --ttf-dir /tmp/liberation-fonts-ttf-2.1.5

Output: lib_sans_small.h / lib_sans_body.h / lib_sans_emph.h /
lib_sans_hero.h (bitmap pool + glyph table + ui_font_t descriptor each,
ASCII 0x20..0x7E only) and
ui_font_metrics.h (cap/ascent/descent as #defines for layout arithmetic).
The Adafruit-GFX table LAYOUT is an open de-facto standard implemented
fresh here; no Adafruit code and no third-party font table is copied.
"""

import argparse
import hashlib
import os
import sys

import freetype

DPI = 141  # Adafruit fontconvert's DPI convention; keeps pt sizes comparable

TTF_SHA256 = {
    "LiberationSans-Regular.ttf":
        "76d04c18ea243f426b7de1f3ad208e927008f961dc5945e5aad352d0dfde8ee8",
    "LiberationSans-Bold.ttf":
        "788abee4c806d660e8aee46689dd8540cd4bb98da03dcc9d171ce3efd99a9173",
}

FONTS = [
    # (output stem, C identifier, ttf, pt, role comment)
    ("lib_sans_small", "lib_sans_small", "LiberationSans-Regular.ttf", 9.0,
     "small -- status strip and the settings MAC line (chrome, not content)"),
    ("lib_sans_body", "lib_sans_body", "LiberationSans-Regular.ttf", 12.0,
     "body -- lists, previews, transcripts, settings, banner, status messages"),
    ("lib_sans_emph", "lib_sans_emph", "LiberationSans-Bold.ttf", 15.0,
     "emphasis -- list titles"),
    ("lib_sans_hero", "lib_sans_hero", "LiberationSans-Bold.ttf", 18.5,
     "hero -- outcome words (Noted/Done), REC"),
]

FIRST, LAST = 0x20, 0x7E


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def render_font(ttf_path, pt):
    """Render ASCII FIRST..LAST monochrome; return (glyphs, pool, metrics).

    glyphs: list of dicts {offset,w,h,xadv,xo,yo}; pool: bytes of packed
    bitmaps, MSB-first, each glyph starting on a byte boundary (the
    GFX bitmapOffset convention)."""
    face = freetype.Face(ttf_path)
    face.set_char_size(int(round(pt * 64)), 0, DPI, 0)
    glyphs, pool = [], bytearray()
    asc = desc = cap = 0
    for code in range(FIRST, LAST + 1):
        face.load_char(chr(code), freetype.FT_LOAD_TARGET_MONO)
        face.glyph.render(freetype.FT_RENDER_MODE_MONO)
        bm = face.glyph.bitmap
        w, h = bm.width, bm.rows
        top, left = face.glyph.bitmap_top, face.glyph.bitmap_left
        xadv = face.glyph.advance.x >> 6
        # repack FreeType's padded rows into the GFX continuous bit stream
        bits = []
        for row in range(h):
            for col in range(w):
                byte = bm.buffer[row * bm.pitch + col // 8]
                bits.append((byte >> (7 - (col & 7))) & 1)
        packed = bytearray()
        for i in range(0, len(bits), 8):
            b = 0
            for j, bit in enumerate(bits[i:i + 8]):
                b |= bit << (7 - j)
            packed.append(b)
        offset = len(pool)
        assert offset <= 0xFFFF, "bitmap pool exceeds uint16 bitmapOffset"
        assert -128 <= left <= 127 and -128 <= -top <= 127
        assert 0 <= w <= 255 and 0 <= h <= 255 and 0 <= xadv <= 255
        pool += packed
        glyphs.append({"offset": offset, "w": w, "h": h,
                       "xadv": xadv, "xo": left, "yo": -top})
        if h:
            asc = max(asc, top)
            desc = max(desc, h - top)
        if code == ord("H"):
            cap = h
    yadv = face.size.height >> 6
    return glyphs, bytes(pool), {"cap": cap, "asc": asc, "desc": desc,
                                 "yadv": yadv}


def emit_header(out_dir, stem, ident, ttf_name, pt, role, glyphs, pool, m):
    lines = []
    lines.append("/* Generated by gen_gfx_font.py -- DO NOT EDIT BY HAND.")
    lines.append(" *")
    lines.append(" * %s" % role)
    lines.append(" * Face: %s (Liberation fonts 2.1.5, SIL OFL 1.1 --" % ttf_name)
    lines.append(" *       see LICENSE.liberation beside this file)")
    lines.append(" * TTF sha256: %s" % TTF_SHA256[ttf_name])
    lines.append(" * Upstream: github.com/liberationfonts/liberation-fonts"
                 " release 2.1.5,")
    lines.append(" *   liberation-fonts-ttf-2.1.5.tar.gz sha256")
    lines.append(" *   7191c669bf38899f73a2094ed00f7b800553364f90e2637010a"
                 "69c0e268f25d0")
    lines.append(" * Rendered: %.1f pt at %d dpi, FT_LOAD_TARGET_MONO" % (pt, DPI))
    lines.append(" * Coverage: ASCII 0x20..0x7E (95 glyphs)")
    lines.append(" * Metrics: cap %d, ascent %d, descent %d, yAdvance %d"
                 % (m["cap"], m["asc"], m["desc"], m["yadv"]))
    lines.append(" * Table: %d B bitmap pool + %d B glyph table"
                 % (len(pool), len(glyphs) * 8))
    lines.append(" */")
    lines.append("")
    lines.append("static const uint8_t %s_bitmap[%d] = {" % (ident, len(pool)))
    for i in range(0, len(pool), 12):
        chunk = ", ".join("0x%02x" % b for b in pool[i:i + 12])
        lines.append("    %s," % chunk)
    lines.append("};")
    lines.append("")
    lines.append("static const ui_glyph_t %s_glyph[%d] = {" % (ident, len(glyphs)))
    for i, g in enumerate(glyphs):
        ch = chr(FIRST + i)
        cch = ch if ch not in "\\'" else " "
        lines.append("    { %5d, %3d, %3d, %3d, %4d, %4d },  /* 0x%02x '%s' */"
                     % (g["offset"], g["w"], g["h"], g["xadv"], g["xo"],
                        g["yo"], FIRST + i, cch))
    lines.append("};")
    lines.append("")
    lines.append("static const ui_font_t %s = {" % ident)
    lines.append("    %s_bitmap, %s_glyph," % (ident, ident))
    lines.append("    0x%02x, 0x%02x, %d, %d, %d, %d," % (FIRST, LAST,
                 m["yadv"], m["cap"], m["asc"], m["desc"]))
    lines.append("};")
    lines.append("")
    path = os.path.join(out_dir, stem + ".h")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf-dir", required=True,
                    help="directory containing the Liberation Sans TTFs")
    ap.add_argument("--out-dir", default=os.path.dirname(os.path.abspath(__file__)))
    args = ap.parse_args()

    metrics = {}
    for stem, ident, ttf_name, pt, role in FONTS:
        ttf = os.path.join(args.ttf_dir, ttf_name)
        got = sha256_file(ttf)
        if got != TTF_SHA256[ttf_name]:
            sys.exit("sha256 mismatch for %s:\n  got  %s\n  want %s"
                     % (ttf, got, TTF_SHA256[ttf_name]))
        glyphs, pool, m = render_font(ttf, pt)
        # the renderer's '?' fallback and the layout arithmetic rely on these
        assert all(g["xo"] >= -1 for g in glyphs), "unexpected left overhang"
        path = emit_header(args.out_dir, stem, ident, ttf_name, pt, role,
                           glyphs, pool, m)
        metrics[stem] = m
        min_adv = min(g["xadv"] for g in glyphs if g["xadv"])
        print("%s: %d glyphs, %d B pool, cap %d asc %d desc %d yadv %d "
              "min_adv %d -> %s" % (ident, len(glyphs), len(pool), m["cap"],
                                    m["asc"], m["desc"], m["yadv"], min_adv,
                                    path))

    mm = []
    mm.append("/* Generated by gen_gfx_font.py -- DO NOT EDIT BY HAND.")
    mm.append(" * Cap-height / ascent / descent of the three generated faces,")
    mm.append(" * as compile-time constants for layout arithmetic and its")
    mm.append(" * _Static_asserts (the tables themselves stay private to fb.c).")
    mm.append(" */")
    mm.append("#ifndef UI_FONT_METRICS_H")
    mm.append("#define UI_FONT_METRICS_H")
    mm.append("")
    for stem, name in (("lib_sans_small", "SMALL"), ("lib_sans_body", "BODY"),
                       ("lib_sans_emph", "EMPH"), ("lib_sans_hero", "HERO")):
        m = metrics[stem]
        mm.append("#define UI_FONT_%s_CAP  %d" % (name, m["cap"]))
        mm.append("#define UI_FONT_%s_ASC  %d" % (name, m["asc"]))
        mm.append("#define UI_FONT_%s_DESC %d" % (name, m["desc"]))
        mm.append("")
    mm.append("#endif")
    mm.append("")
    with open(os.path.join(args.out_dir, "ui_font_metrics.h"), "w") as f:
        f.write("\n".join(mm))
    print("ui_font_metrics.h written")


if __name__ == "__main__":
    main()
