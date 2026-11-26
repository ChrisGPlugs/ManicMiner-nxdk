#!/usr/bin/env python3
"""Build an XBMC4Gamers _resources artwork set for ManicMinerX.

usage: make_artwork.py <templates dir> <Artwork dir> <clean cover png> <out dir>

<templates dir>  the XBMC4Gamers "Create _Resources Artwork" folder (PSDs, not in this repo)
<Artwork dir>    the repo's Artwork/ folder (4.gif, 4 (1).gif)
<clean cover>    output of clean_cover.py (Software Projects art, advert text removed)

Example, from the repo root:
  python3 tools/clean_cover.py Artwork/ManicMiner.jpg artwork_out/cover_clean.png
  python3 tools/make_artwork.py "/path/to/Create _Resources Artwork" Artwork \
      artwork_out/cover_clean.png artwork_out/ManicMinerX
  (cd artwork_out && zip -r ManicMinerX_artwork.zip ManicMinerX)

The output folder holds default.xml, screenshots/ and artwork/; its contents go in
the game's _resources folder for XBMC4Gamers. Also writes scan_full.png (the flat
1610x1080 cover) and disc_full.png next to the output folder.
The output is made from the publishers' cover art: keep it out of the repo and releases.

Produces <out dir>/default.xml, screenshots/Screenshot-1.jpg and artwork/*
with the same names, sizes and modes as the Abbayex example set.
Needs: pillow, numpy, psd-tools.
"""
import os
import sys
import textwrap

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from psd_tools import PSDImage
from psd_tools.constants import Tag

TPL, ART, COVER, OUT = sys.argv[1:5]
FONT_DIR = "/usr/share/fonts/truetype/google-fonts"
BOLD = os.path.join(FONT_DIR, "Poppins-Bold.ttf")
MED = os.path.join(FONT_DIR, "Poppins-Medium.ttf")
REG = os.path.join(FONT_DIR, "Poppins-Regular.ttf")

SPECCY_YELLOW = (215, 215, 0)
BG_TOP, BG_BOT = (28, 40, 50), (8, 12, 16)

# ---------------------------------------------------------------- helpers

_psd = {}


def layers(name):
    """Return the PSD's non-group layers as a list (bottom to top)."""
    if name not in _psd:
        _psd[name] = PSDImage.open(os.path.join(TPL, name))
    p = _psd[name]
    return p, [l for l in p.descendants() if not l.is_group()]


def layer_img(psdname, idx_or_name, canvas=None):
    """A layer's pixels placed on a full transparent canvas, with layer opacity applied."""
    p, ls = layers(psdname)
    if isinstance(idx_or_name, int):
        l = [x for x in p.descendants()][idx_or_name]
    else:
        l = [x for x in ls if x.name == idx_or_name][0]
    size = canvas or p.size
    t = l.topil().convert("RGBA")
    # layer pixels on a full-size canvas
    c = Image.new("RGBA", size, (0, 0, 0, 0))
    c.paste(t, (l.left, l.top))
    a = np.array(c.getchannel("A")).astype(np.float32)
    # opacity x fill opacity (fill 0 = only layer effects, which we don't render)
    fill = 255
    tb = l.tagged_blocks
    if tb is not None and tb.get_data(Tag.BLEND_FILL_OPACITY) is not None:
        fill = tb.get_data(Tag.BLEND_FILL_OPACITY)
    a *= (l.opacity / 255.0) * (fill / 255.0)
    # layer mask
    if l.has_mask() and not l.mask.disabled:
        mk = l.mask.topil()
        m = Image.new("L", size, l.mask.background_color)
        if mk is not None:
            m.paste(mk.convert("L"), (l.mask.left, l.mask.top))
        a *= np.array(m).astype(np.float32) / 255.0
    c.putalpha(Image.fromarray(a.clip(0, 255).astype(np.uint8)))
    return c


def persp_coeffs(dst_quad, w, h):
    """Coefficients for Image.transform mapping dst quad -> source rect (0,0,w,h)."""
    src = [(0, 0), (w, 0), (w, h), (0, h)]
    A, B = [], []
    for (x, y), (u, v) in zip(dst_quad, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); B.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); B.append(v)
    return np.linalg.solve(np.array(A, float), np.array(B, float)).tolist()


def warp(img, quad, size):
    """Warp img onto quad (TL, TR, BR, BL) of a transparent canvas of `size`."""
    img = img.convert("RGBA")
    out = img.transform(size, Image.PERSPECTIVE, persp_coeffs(quad, *img.size),
                        Image.BICUBIC, fillcolor=(0, 0, 0, 0))
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).polygon(quad, fill=255)
    a = np.minimum(np.array(out.getchannel("A")), np.array(m))
    out.putalpha(Image.fromarray(a))
    return out


def gradient(size, top, bot):
    w, h = size
    t = np.linspace(0, 1, h)[:, None, None]
    arr = (np.array(top) * (1 - t) + np.array(bot) * t) * np.ones((1, w, 1))
    return Image.fromarray(arr.astype(np.uint8), "RGB").convert("RGBA")


def fit_box(img, box_w, box_h):
    """Scale RGBA img (cropped to its alpha) to fit box, centred on a transparent box."""
    img = img.crop(img.getchannel("A").getbbox())
    s = min((box_w - 4) / img.width, (box_h - 4) / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    c = Image.new("RGBA", (box_w, box_h), (0, 0, 0, 0))
    c.alpha_composite(img, ((box_w - img.width) // 2, (box_h - img.height) // 2))
    return c


def save_png_p(img, path):
    """Palette PNG with alpha, like the reference set."""
    img.quantize(256, method=Image.Quantize.FASTOCTREE,
                 dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)


def save_jpg(img, path, q=90):
    img.convert("RGB").save(path, quality=q, optimize=True)


def speccy_screen(gifname):
    """Native 256x192 Spectrum screen from a 1024x768 (x4) capture."""
    im = Image.open(os.path.join(ART, gifname))
    im.seek(0)
    im = im.convert("RGB")
    full = Image.new("RGB", (1024, 768), (0, 0, 0))
    full.paste(im.crop((0, 0, 1024, min(768, im.height))), (0, 0))
    return full.resize((256, 192), Image.NEAREST)


def font(path, size):
    return ImageFont.truetype(path, size)


# ---------------------------------------------------------------- sources

cover = Image.open(COVER).convert("RGBA")          # 608x874 Software Projects art
cavern = speccy_screen("4 (1).gif")                 # Central Cavern
title_scr = speccy_screen("4.gif")                  # title screen


def title_logo():
    """Cut the rocky MANIC MINER lettering out of the cover sky."""
    crop = cover.crop((0, 0, 608, 292)).convert("RGB")
    hsv = np.array(crop.convert("HSV")).astype(int)
    h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    warm = ((h < 45) | (h > 245)) & (s > 60) & (v > 60)
    m = Image.fromarray((warm * 255).astype(np.uint8))
    near = np.array(m.filter(ImageFilter.MaxFilter(9))) > 0
    dark = v < 90
    mask = warm | (dark & near)
    m = Image.fromarray((mask * 255).astype(np.uint8))
    m = m.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))
    m = m.filter(ImageFilter.GaussianBlur(0.8))
    out = crop.convert("RGBA")
    out.putalpha(m)
    return out.crop(m.getbbox())


LOGO = title_logo()

# ---------------------------------------------------------------- the scan (1610x1080)
SCAN = "_Blank Scan.psd"
W, H = 1610, 1080
BACK = (0, 0, 762, 1080)
SPINE = (762, 0, 848, 1080)
FRONT = (848, 0, 1610, 1080)


def build_front():
    f = Image.new("RGBA", (762, 1080), (0, 0, 0, 255))
    art = cover.resize((762, round(874 * 762 / 608)), Image.LANCZOS)   # 762x1095
    f.alpha_composite(art.crop((0, 0, 762, 973)), (0, 107))
    banner = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    banner.alpha_composite(layer_img(SCAN, "Banner Background"))
    banner.alpha_composite(layer_img(SCAN, "Banner TXT"))
    f.alpha_composite(banner.crop(FRONT))
    return f


def build_spine():
    s = gradient((86, 1080), (20, 20, 20), (0, 0, 0))
    top = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    top.alpha_composite(layer_img(SCAN, "Spine Top"))
    top.alpha_composite(layer_img(SCAN, "Spine TXT"))
    s.alpha_composite(top.crop(SPINE))
    # title, reading top to bottom like the template's APPLICATIONS text
    fnt = font(BOLD, 50)
    txt = "MANIC MINER"
    tw = int(fnt.getlength(txt))
    t = Image.new("RGBA", (tw + 10, 70), (0, 0, 0, 0))
    ImageDraw.Draw(t).text((5, 2), txt, font=fnt, fill=SPECCY_YELLOW + (255,))
    t = t.rotate(-90, expand=True)
    s.alpha_composite(t, ((86 - t.width) // 2 + 2, 270))
    fnt2 = font(MED, 22)
    t2 = Image.new("RGBA", (300, 34), (0, 0, 0, 0))
    ImageDraw.Draw(t2).text((2, 0), "48K  +  128", font=fnt2, fill=(230, 230, 230, 255))
    t2 = t2.rotate(-90, expand=True)
    s.alpha_composite(t2, ((86 - t2.width) // 2 + 2, 880))
    return s


def framed_shot(scr, w):
    """Spectrum screen with a yellow frame, scaled to width w (pixel-sharp)."""
    k = 4
    big = scr.resize((256 * k, 192 * k), Image.NEAREST)
    h = round(w * 192 / 256)
    shot = big.resize((w, h), Image.LANCZOS).convert("RGBA")
    fr = Image.new("RGBA", (w + 12, h + 12), SPECCY_YELLOW + (255,))
    fr.alpha_composite(Image.new("RGBA", (w + 4, h + 4), (0, 0, 0, 255)), (4, 4))
    fr.alpha_composite(shot, (6, 6))
    return fr


def build_back():
    b = gradient((762, 1080), BG_TOP, BG_BOT)
    
    hb = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    hb.alpha_composite(layer_img(SCAN, 23))           # HOMEBREW header (hidden in template)
    b.alpha_composite(hb.crop(BACK))

    lg = LOGO.resize((560, round(LOGO.height * 560 / LOGO.width)), Image.LANCZOS)
    b.alpha_composite(lg, ((762 - lg.width) // 2, 122))
    y0 = 122 + lg.height + 22

    # blurb panel
    px, py, pw, ph = 26, y0, 380, 600
    panel = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    d = ImageDraw.Draw(panel)
    d.rounded_rectangle((0, 0, pw - 1, ph - 1), 14, fill=(0, 0, 0, 190),
                        outline=SPECCY_YELLOW + (255,), width=3)
    body = font(REG, 19)
    head = font(BOLD, 21)
    y = 18
    paras = [
        ("head", "Down the mine with Miner Willy"),
        ("body", "Prospecting near Surbiton, Willy finds a long-forgotten mine still "
                 "worked by the robots of a vanished civilisation. Collect every flashing "
                 "key in each of the 20 caverns, then reach the portal before your air "
                 "runs out. Watch out for poisonous pansies, mutant telephones and the "
                 "manic mining robots themselves."),
        ("head", "Two games on one disc"),
        ("body", "The original Bug-Byte Manic Miner and Manic Miner 128 with AY music "
                 "and its in-game menu. Press Back twice on the title screen to switch. "
                 "High scores are saved to the Xbox hard disk."),
    ]
    for kind, text in paras:
        if kind == "head":
            d.text((18, y), text, font=head, fill=SPECCY_YELLOW + (255,))
            y += 34
        else:
            for line in textwrap.wrap(text, 36):
                d.text((18, y), line, font=body, fill=(235, 235, 235, 255))
                y += 26
            y += 16
    ph = y + 4
    panel = panel.crop((0, 0, pw, ph))
    d = ImageDraw.Draw(panel)
    d.rounded_rectangle((0, 0, pw - 1, ph - 1), 14, outline=SPECCY_YELLOW + (255,), width=3)
    b.alpha_composite(panel, (px, py))

    # two screenshots on the right
    s1 = framed_shot(cavern, 310)
    s2 = framed_shot(title_scr, 310)
    sx = 762 - 26 - s1.width
    b.alpha_composite(s1, (sx, y0))
    b.alpha_composite(s2, (sx, y0 + s1.height + 24))

    legal = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for i in (10, 11, 12, 13):  # rating box, legal text x2, Microsoft logo
        legal.alpha_composite(layer_img(SCAN, i))
    b.alpha_composite(legal.crop(BACK))
    return b


def build_scan():
    scan = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    scan.alpha_composite(build_back(), (0, 0))
    scan.alpha_composite(build_spine(), (762, 0))
    scan.alpha_composite(build_front(), (848, 0))
    return scan


# ---------------------------------------------------------------- disc (904x904)
DVD = "_Xbox DVD.psd"


def build_disc():
    S = 904
    cx = cy = 454
    R = 432
    disc = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    art = Image.new("RGBA", (S, S), (62, 84, 98, 255))       # cover sky colour
    a = cover.resize((864, round(874 * 864 / 608)), Image.LANCZOS)
    art.alpha_composite(a, (22, 236))
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
    disc.paste(art, (0, 0), m)
    # Xbox banner cap
    disc.alpha_composite(layer_img(DVD, 40))
    border = layer_img(DVD, 41)
    ba = border.getchannel("A")
    border = Image.new("RGBA", (S, S), (235, 235, 235, 255))
    border.putalpha(ba)
    disc.alpha_composite(border)
    disc.alpha_composite(layer_img(DVD, 42))
    # spindle rings
    for i in (34, 35, 36, 37, 38):
        try:
            disc.alpha_composite(layer_img(DVD, i))
        except Exception:
            pass
    # rim and hole
    d = ImageDraw.Draw(disc)
    d.ellipse((cx - R, cy - R, cx + R, cy + R), outline=(200, 200, 200, 255), width=3)
    alpha = np.array(disc.getchannel("A"))
    yy, xx = np.mgrid[0:S, 0:S]
    rr = np.hypot(xx - cx + 0.5, yy - cy + 0.5)
    alpha[rr < 56] = 0
    alpha[rr > R + 1.5] = 0
    disc.putalpha(Image.fromarray(alpha))
    return disc


# ---------------------------------------------------------------- 3D renders


def render_3d(scan, tpl, spine_quad, front_quad, back_quad=None, under=(), over_from=None):
    p, _ = layers(tpl)
    c = Image.new("RGBA", p.size, (0, 0, 0, 0))
    for i in under:
        c.alpha_composite(layer_img(tpl, i))
    if back_quad:
        c.alpha_composite(warp(scan.crop(BACK), back_quad, p.size))
    c.alpha_composite(warp(scan.crop(SPINE), spine_quad, p.size))
    c.alpha_composite(warp(scan.crop(FRONT), front_quad, p.size))
    all_l = list(p.descendants())
    for i, l in enumerate(all_l):
        if i >= over_from and not l.is_group() and l.visible:
            c.alpha_composite(layer_img(tpl, i))
    return c


def build_icon(scan):
    return render_3d(scan, "Default - 3D Covers.psd",
                     [(388, 43), (467, 14), (467, 1523), (388, 1494)],
                     [(467, 14), (1249, 163), (1249, 1373), (467, 1523)],
                     under=(1,), over_from=2)


def build_dual(scan):
    return render_3d(scan, "Default - Dual Covers.psd",
                     [(726, 43), (805, 14), (805, 1523), (726, 1494)],
                     [(805, 14), (1587, 163), (1587, 1373), (805, 1523)],
                     back_quad=[(21, 195), (725, 59), (725, 1478), (21, 1343)],
                     under=(1,), over_from=2)


def build_open(scan, disc):
    tpl = "Default - Open Case.psd"
    p, _ = layers(tpl)
    size = p.size
    back = Image.new("RGBA", size, (0, 0, 0, 0))
    for i in (4, 8, 9):                       # back case, disc holder lower
        back.alpha_composite(layer_img(tpl, i))
    dd = 800
    dsc = disc.resize((round(904 * dd / 864),) * 2, Image.LANCZOS)
    back.alpha_composite(dsc, (803 - dsc.width // 2, 806 - dsc.height // 2))
    for i in (13, 14, 15):                    # droplet hub
        back.alpha_composite(layer_img(tpl, i))
    full = back.copy()
    for i in (17, 18):                        # back-case shade, lid
        full.alpha_composite(layer_img(tpl, i))
    full.alpha_composite(warp(scan.crop(FRONT), [(374, 197), (998, 27), (998, 1582), (374, 1412)], size))
    full.alpha_composite(layer_img(tpl, 20))  # lid shine
    return back, full


def build_banner(scan):
    """Box lying on its back: spine facing us, front cover seen from above."""
    k = 4
    Wb, Hb = 448 * k, 84 * k
    c = Image.new("RGBA", (Wb, Hb), (0, 0, 0, 0))
    x0, x1 = 8 * k, 440 * k
    top_y0, top_y1, sp_y1 = 12 * k, 40 * k, 72 * k
    inset = 10 * k
    green, dark = (60, 110, 20, 255), (35, 70, 10, 255)
    d = ImageDraw.Draw(c)
    # case body (green rim)
    d.polygon([(x0 + inset, top_y0 - 2 * k), (x1 - inset, top_y0 - 2 * k), (x1 + 2, top_y1),
               (x1 + 2, sp_y1 + 2 * k), (x0 - 2, sp_y1 + 2 * k), (x0 - 2, top_y1)], fill=dark)
    # top face: front cover rotated so its top edge is at the left, squashed
    fr = scan.crop(FRONT).rotate(90, expand=True)          # 1080x762
    top = warp(fr, [(x0 + inset, top_y0), (x1 - inset, top_y0), (x1, top_y1), (x0, top_y1)], (Wb, Hb))
    c.alpha_composite(top)
    # spine face
    sp = scan.crop(SPINE).rotate(90, expand=True)          # 1080x86, top at left
    sp = sp.resize((x1 - x0, sp_y1 - top_y1), Image.LANCZOS)
    c.alpha_composite(sp, (x0, top_y1))
    d = ImageDraw.Draw(c)
    d.line([(x0, top_y1), (x1, top_y1)], fill=(150, 200, 90, 255), width=k)
    d.rectangle((x0, top_y1, x1, sp_y1), outline=green, width=2 * k)
    d.polygon([(x0 + inset, top_y0), (x1 - inset, top_y0), (x1, top_y1), (x0, top_y1)],
              outline=green, width=2 * k)
    # shading on the top face
    sh = np.zeros((Hb, Wb), np.uint8)
    sh[top_y0:top_y1, :] = np.linspace(110, 30, top_y1 - top_y0)[:, None].astype(np.uint8)
    shade = Image.new("RGBA", (Wb, Hb), (0, 0, 0, 0))
    shade.putalpha(Image.fromarray(sh))
    mask = top.getchannel("A")
    shade.putalpha(Image.fromarray(np.minimum(np.array(shade.getchannel("A")), np.array(mask))))
    c.alpha_composite(shade)
    return c.resize((448, 84), Image.LANCZOS)


# ---------------------------------------------------------------- main
XML = """<synopsis>
<sourcename>ManicMinerX v1.0.0</sourcename>
<foldername>ManicMinerX</foldername>
<title>Manic Miner</title>
<developer>Matthew Smith / ChrisGPlugs (Xbox port)</developer>
<publisher>Homebrew</publisher>
<features_general>Single-player</features_general>
<features_online/>
<esrb>Not Rated</esrb>
<esrb_descriptors>Not Rated</esrb_descriptors>
<genre>Platformer</genre>
<release_date>1983</release_date>
<year>1983</year>
<rating>5.0</rating>
<platform>Xbox, ZX Spectrum</platform>
<exclusive>No</exclusive>
<titleid>4D4D0058</titleid>
<overview>ManicMinerX is a native Xbox port of Matthew Smith's 1983 ZX Spectrum classic, translated from the original Z80 code and running at the Spectrum's real speed. Guide Miner Willy through 20 caverns, collecting every flashing key and reaching the portal before the air runs out, while avoiding poisonous pansies, mutant telephones and manic mining robots. Both the original Bug-Byte game and Manic Miner 128 (with AY music and an in-game menu) are included: press Back twice on the title screen to switch between them. High scores are saved to the Xbox hard disk.</overview>
</synopsis>
"""


def main():
    art = os.path.join(OUT, "artwork")
    shots = os.path.join(OUT, "screenshots")
    os.makedirs(art, exist_ok=True)
    os.makedirs(shots, exist_ok=True)

    scan = build_scan()
    scan.convert("RGB").save(os.path.join(OUT, "..", "scan_full.png"))
    front = scan.crop(FRONT).convert("RGB")
    backc = scan.crop(BACK).convert("RGB")

    # covers
    save_jpg(front.resize((200, 283), Image.LANCZOS), os.path.join(art, "poster.jpg"))
    ps = front.resize((100, 142), Image.LANCZOS)
    save_jpg(ps, os.path.join(art, "poster_small.jpg"))
    save_jpg(ps.filter(ImageFilter.GaussianBlur(2.5)), os.path.join(art, "poster_small_blurred.jpg"))
    save_jpg(front.resize((362, 512), Image.LANCZOS), os.path.join(art, "synopsis.jpg"))
    save_jpg(backc.resize((362, 512), Image.LANCZOS), os.path.join(art, "alt_synopsis.jpg"))
    save_jpg(front.crop((0, 107, 762, 107 + 433)).resize((227, 129), Image.LANCZOS),
             os.path.join(art, "thumb.jpg"))

    # disc
    disc = build_disc()
    disc.save(os.path.join(OUT, "..", "disc_full.png"))
    cd = disc.resize((256, 256), Image.LANCZOS)
    save_png_p(cd, os.path.join(art, "cd.png"))
    blk = Image.new("RGBA", (256, 256), (0, 0, 0, 255))
    blk.alpha_composite(cd)
    save_jpg(blk.resize((64, 64), Image.LANCZOS), os.path.join(art, "cd_small.jpg"))

    # cases
    save_png_p(fit_box(build_icon(scan), 149, 263), os.path.join(art, "icon.png"))
    save_png_p(fit_box(build_dual(scan), 256, 256), os.path.join(art, "dual3d.png"))
    back, full = build_open(scan, disc)
    save_png_p(back.crop((390, 198, 1242, 1412)).resize((200, 283), Image.LANCZOS),
               os.path.join(art, "cdposter.png"))
    save_png_p(fit_box(full, 149, 263), os.path.join(art, "opencase.png"))
    save_png_p(build_banner(scan), os.path.join(art, "banner.png"))

    # fanart: Central Cavern playfield, pixel-sharp
    big = cavern.resize((1024, 768), Image.NEAREST).crop((0, 0, 1024, 544))
    fan = Image.new("RGB", (640, 360), (0, 0, 0))
    fan.paste(big.resize((640, 340), Image.LANCZOS), (0, 10))
    save_jpg(fan, os.path.join(art, "fanart.jpg"))
    save_jpg(fan.resize((360, 203), Image.LANCZOS), os.path.join(art, "fanart_thumb.jpg"))
    save_jpg(fan.resize((220, 124), Image.LANCZOS).filter(ImageFilter.GaussianBlur(3)),
             os.path.join(art, "fanart-blur.jpg"))
    save_jpg(Image.new("RGB", (8, 8), (20, 28, 36)), os.path.join(art, "fog.jpg"))

    # screenshot: full screen with border, 384x288
    sc = Image.new("RGB", (1280, 960), (0, 0, 0))
    sc.paste(cavern.resize((1024, 768), Image.NEAREST), (128, 96))
    save_jpg(sc.resize((384, 288), Image.LANCZOS), os.path.join(shots, "Screenshot-1.jpg"))

    with open(os.path.join(OUT, "default.xml"), "w", encoding="utf-8", newline="\n") as f:
        f.write(XML)


if __name__ == "__main__":
    main()
