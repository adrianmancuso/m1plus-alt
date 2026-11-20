#!/usr/bin/env python3
"""The colour schemes' sheet (docs/modules/accent.md), drawn through the real model: every colour goes through palette.map_colour and every image through palette.render_image
(tools/palette.py, which test/pod/palette.py compares with the C of patch/palette.c), with no patching. Approximate (PIL, not AWTK); the cover is a synthetic sample.

  out/preview/schemes/schemes.png        rows = Classic, the seven riso schemes, Greyscale; columns = the M1+ Music menu (Albums pressed) and Now Playing, by day and by night
  out/preview/schemes/scheme-N-NAME.png  one scheme: menu and Now Playing by day, then by night

A riso scheme's day is the device clock (07:00 to 18:59): cream paper and the scheme's ink; by night its dark paper and cream ink. Now Playing's window takes the cover tone through
palette.tone (Greyscale: the luma grey; a riso scheme by day: the neutral tone).

Usage: uv run --locked python -I tools/preview/preview_schemes.py WORKDIR [OUT_DIR]     (default out/preview/schemes/)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from PIL import Image, ImageDraw
import build, palette as PM, preview_icons as PI, preview_accent as PA, preview_nowplaying as NP

W, H = PA.W, PA.H


def cover_art():
    """A synthetic, colourful square sample cover (not a real record)."""
    n = 400
    im = Image.new('RGB', (n, n)); px = im.load()
    for y in range(n):
        for x in range(n): px[x, y] = (230 - y // 3, 90 + x // 4, 40 + (x + y) // 5)
    d = ImageDraw.Draw(im)
    d.ellipse((90, 90, 310, 310), fill=(20, 60, 140)); d.ellipse((160, 160, 240, 240), fill=(250, 210, 60)); d.rectangle((0, 330, n, 360), fill=(200, 30, 60))
    return im


def grey(im):
    """Greyscale greys a cover (palette.grey_pixel, the C grey_image)."""
    im = im.convert('RGBA'); out = Image.new('RGBA', im.size)
    out.putdata([PM.grey_pixel(p) for p in im.getdata()])
    return out


def nowplaying(raw, pal, cover):
    """Now Playing with a cover: its pixels as the image hook gives them, its window the cover's tone as pal_tone gives it."""
    class P(NP.Painter):
        def prop(self, wd, prop, state='normal'):
            return pal.css(super().prop(wd, prop, state))

        def image(self, name):
            im = super().image(name)
            return PM.render_image(name, im, pal.bg, pal.ac, pal.scheme, pal.day) if im is not None else None
    avg = cover.convert('RGB').resize((1, 1), Image.BOX).getpixel((0, 0))
    p = P(raw); p.playing = True; p.cover_img = grey(cover) if pal.scheme == 'greyscale' else cover
    p.tint = NP.tone(*avg) if pal.scheme == 'classic' else PM.tone(NP.tone(*avg), pal.scheme) if pal.scheme == 'greyscale' or not pal.day else NP.MN.NP_NEUTRAL
    root = NP.awtk_ui.parse(open(os.path.join(raw, 'ui', 'playing_page.bin'), 'rb').read())
    return p.render(root, 0)


def hx(c):
    return '#%02X%02X%02X' % tuple(c[:3])


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    work = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(build.ROOT, 'out', 'preview', 'schemes')
    os.makedirs(out_dir, exist_ok=True)
    s = PI.Side(work, True, 'en_US'); s.mark = lambda *a: None
    raw = os.path.join(work, PI.RAW)
    art = cover_art()
    pad, lab, top = 14, 22, 44
    shots = []   # (scheme, [(menu, nowplaying) by day, by night])
    for sc in PM.SCHEMES:
        row = []
        for day in (True, False):
            pal = PA.Palette('black', 'neutral', scheme=sc, day=day)
            row.append((PA.menu_screen(s, pal), nowplaying(raw, pal, art)))
        shots.append((sc, row))
    cols = ['Menu, day', 'Now Playing, day', 'Menu, night', 'Now Playing, night']
    sheet = Image.new('RGB', (4 * (W + pad) + pad, top + len(shots) * (H + pad + lab)), (0x2A,) * 3); d = ImageDraw.Draw(sheet)
    d.text((pad, 10), 'Colour schemes (palette.map_colour / render_image); day = 07:00 to 18:59, the sample cover is synthetic', fill=(0xEE,) * 3, font=PI.ANN_B)
    for j, c in enumerate(cols): d.text((pad + j * (W + pad), 28), c, fill=(0xAA,) * 3, font=PI.ANN)
    for i, (sc, row) in enumerate(shots):
        y = top + i * (H + pad + lab)
        if PM.riso(sc): label = f'{sc}   paper {hx(PM.SCHEME_PAPER)}  ink {hx(PM.scheme_ends(sc)[1])}  accent {hx(PM.scheme_accent(sc))}  night paper {hx(PM.scheme_ends(sc, False)[0])}'
        else: label = sc + ('   (today: Black + Neutral)' if sc == 'classic' else '   (Black + Neutral, covers in grey)')
        d.text((pad, y), label, fill=(0xEE,) * 3, font=PI.ANN_B)
        pair = Image.new('RGB', (4 * W + 5 * pad, H + 2 * pad), (0x2A,) * 3)
        for k, im in enumerate([x for r in row for x in r]):
            im = im.convert('RGB').crop((0, 0, W, H))
            sheet.paste(im, (pad + k * (W + pad), y + lab)); pair.paste(im, (pad + k * (W + pad), pad))
        pair.save(os.path.join(out_dir, f'scheme-{i}-{sc}.png'), optimize=True)
    path = os.path.join(out_dir, 'schemes.png')
    sheet.save(path, optimize=True)
    print('wrote', path, sheet.size)


if __name__ == '__main__':
    main()
