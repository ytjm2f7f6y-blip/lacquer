"""Lacquer: the flat art the 3D renders are textured with (forked from Kontur).

Writes into art/build/: the printed legends (black ink and cream ink), the
worn brand lettering, the meter faces, the masking tape (shape, crepe, the
handwriting) and a wear map. Everything is drawn at TEX x the point size.
"""
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

import layout as L

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")
FONTS = os.path.join(HERE, "fonts")
os.makedirs(OUT, exist_ok=True)

TEX = 4                                   # texture pixels per point
W, H = L.W * TEX, L.H * TEX
rng = random.Random(1966)


def font(name, size_pt, weight=None):
    f = ImageFont.truetype(os.path.join(FONTS, name), int(round(size_pt * TEX)))
    if weight is not None:
        try:
            f.set_variation_by_axes([weight])
        except Exception:
            pass
    return f


LEGEND = "BarlowSemiCondensed-SemiBold.ttf"
LEGEND_MED = "BarlowSemiCondensed-Medium.ttf"
LEGEND_BOLD = "BarlowSemiCondensed-Bold.ttf"


def text_centered(d, xy, s, f, fill=255, spacing=0.0):
    """Centre a string at xy (points), with optional letter spacing (em)."""
    x, y = xy[0] * TEX, xy[1] * TEX
    if spacing == 0:
        d.text((x, y), s, font=f, fill=fill, anchor="mm")
        return
    widths = [f.getlength(ch) for ch in s]
    extra = spacing * f.size
    total = sum(widths) + extra * (len(s) - 1)
    cur = x - total / 2
    for ch, w in zip(s, widths):
        d.text((cur + w / 2, y), ch, font=f, fill=fill, anchor="mm")
        cur += w + extra


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


def norm_to_deg(n):
    return -L.SWEEP + 2 * L.SWEEP * n


# ---------------------------------------------------------------------------
# Printed legends on the cassettes (black ink), as a coverage mask.
def draw_controls(d, controls):
    T = L.TYPE
    f_name = font(LEGEND_BOLD, T["name"])
    f_num = font(LEGEND, T["num"])
    f_sub = font(LEGEND_MED, T["sub"])
    f_tog = font(LEGEND, T["toggle"])
    for ctl in controls:
        t = ctl["type"]
        if t == "knob":
            r = L.KNOBS[ctl["kind"]]
            x, y = ctl["x"], ctl["y"]
            if ctl["kind"] == "sel":
                for lab in ctl["labels"]:
                    a = lab["a"]
                    p0 = polar(x, y, r + 3, a)
                    p1 = polar(x, y, r + 8.5, a)
                    d.line([(p0[0] * TEX, p0[1] * TEX), (p1[0] * TEX, p1[1] * TEX)], fill=255, width=int(1.2 * TEX))
                    tp = polar(x, y, ctl.get("label_r") or (r + L.LABEL_R), a)
                    if lab["t"].startswith("icon:"):
                        shape_icon(d, tp, lab["t"][5:])
                    else:
                        text_centered(d, tp, lab["t"], f_num)
            else:
                rt0, rt1, rl = r + 3, r + 8, r + L.LABEL_R
                for n in ctl["ticks"]:
                    a = norm_to_deg(n)
                    p0, p1 = polar(x, y, rt0, a), polar(x, y, rt1, a)
                    d.line([(p0[0] * TEX, p0[1] * TEX), (p1[0] * TEX, p1[1] * TEX)], fill=255, width=int(0.95 * TEX))
                for lab in ctl["labels"]:
                    a = norm_to_deg(lab["n"])
                    p = polar(x, y, rl, a)
                    text_centered(d, p, lab["t"], f_num)
            ny = y + r + L.NAME_GAP
            text_centered(d, (x, ny), ctl["name"], f_name, spacing=0.05)
            if ctl.get("sub"):
                text_centered(d, (x, ny + L.SUB_GAP), ctl["sub"], f_sub)
        elif t == "toggle":
            x, y = ctl["x"], ctl["y"]
            text_centered(d, (x, y - 25), ctl["up"], f_tog)
            text_centered(d, (x, y + 25), ctl["down"], f_tog)
            if ctl.get("name"):
                text_centered(d, (x, y + 40), ctl["name"], f_name, spacing=0.05)
        elif t == "lamp" and ctl["id"] == "power":
            text_centered(d, (ctl["x"], ctl["y"] - 16), "POWER", f_sub)
        elif t == "lamp" and ctl["id"].endswith("_idea"):
            text_centered(d, (ctl["x"], ctl["y"] + 13), ctl["legend"], f_sub, spacing=0.12)
        elif t == "lamp" and ctl.get("legend"):
            text_centered(d, (ctl["x"] - 9 - f_sub.getlength(ctl["legend"]) / TEX / 2, ctl["y"]), ctl["legend"], f_sub)



def legends():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    T = L.TYPE
    f_title = font(LEGEND_BOLD, T["title"])
    f_name = font(LEGEND_BOLD, T["name"])
    f_num = font(LEGEND, T["num"])
    f_sub = font(LEGEND_MED, T["sub"])
    f_tog = font(LEGEND, T["toggle"])

    for c in L.CASSETTES:
        text_centered(d, ((c["x0"] + c["x1"]) / 2, 88), c["title"], f_title, spacing=0.14)
        # a printed rule under the title
        x0, x1 = c["x0"] + 18, c["x1"] - 18
        d.line([(x0 * TEX, 100 * TEX), (x1 * TEX, 100 * TEX)], fill=255, width=int(0.8 * TEX))

    draw_controls(d, L.CONTROLS)

    # The screen keys, with a bracket like Kontur's band select.
    kx0 = L.control(L.SCREEN_KEYS[0][0])["x"] - L.KEY_W / 2
    kx1 = L.control(L.SCREEN_KEYS[-1][0])["x"] + L.KEY_W / 2
    ly = L.KEYS_NAME_Y
    xc = L.cx(L.cas("asst"))
    text_centered(d, (xc, ly), "SCREEN", f_name, spacing=0.05)
    for xx in (kx0, kx1):
        d.line([(xx * TEX, (ly - 5) * TEX), (xx * TEX, ly * TEX)], fill=255, width=int(0.7 * TEX))
    d.line([(kx0 * TEX, ly * TEX), ((xc - 30) * TEX, ly * TEX)], fill=255, width=int(0.7 * TEX))
    d.line([((xc + 30) * TEX, ly * TEX), (kx1 * TEX, ly * TEX)], fill=255, width=int(0.7 * TEX))
    # The two readouts' names.
    for r in L.READOUTS:
        text_centered(d, (r["x"], r["y"] + r["h"] / 2 + 11), r["name"], f_name, spacing=0.05)
        text_centered(d, (r["x"], r["y"] + r["h"] / 2 + 21), r["sub"], f_sub)
    # The compare bracket under MATCHED and BLIND.
    bl = L.BYPASS_LABEL
    text_centered(d, (bl["x"], bl["y"]), bl["text"], f_sub, spacing=0.12)
    hw = f_sub.getlength(bl["text"]) / TEX / 2 + 8
    d.line([(bl["x0"] * TEX, (bl["y"] - 4) * TEX), (bl["x0"] * TEX, bl["y"] * TEX), ((bl["x"] - hw) * TEX, bl["y"] * TEX)], fill=255, width=int(0.7 * TEX))
    d.line([((bl["x"] + hw) * TEX, bl["y"] * TEX), (bl["x1"] * TEX, bl["y"] * TEX), (bl["x1"] * TEX, (bl["y"] - 4) * TEX)], fill=255, width=int(0.7 * TEX))
    sl = L.SECT_LABEL
    text_centered(d, (sl["x"], sl["y"]), sl["text"], f_name, spacing=0.05)
    hw = f_name.getlength(sl["text"]) / TEX / 2 + 10
    for xx in (sl["x0"], sl["x1"]):
        d.line([(xx * TEX, (sl["y"] - 5) * TEX), (xx * TEX, sl["y"] * TEX)], fill=255, width=int(0.7 * TEX))
    d.line([(sl["x0"] * TEX, sl["y"] * TEX), ((sl["x"] - hw) * TEX, sl["y"] * TEX)], fill=255, width=int(0.7 * TEX))
    d.line([((sl["x"] + hw) * TEX, sl["y"] * TEX), (sl["x1"] * TEX, sl["y"] * TEX)], fill=255, width=int(0.7 * TEX))
    im.save(os.path.join(OUT, "legend_ink.png"))


ICON_W, ICON_H = 14.5, 9.0


def shape_icon(d, xy, kind):
    """A small printed curve glyph for the shape switch, centred at xy (points)."""
    cxp, cyp = xy
    hw, hh = ICON_W / 2, ICON_H / 2
    pts = []
    n = 60
    for i in range(n + 1):
        u = -1 + 2 * i / n               # -1..1 across the glyph
        if kind == "bell":
            v = 0.55 - 1.35 * math.exp(-(u / 0.32) ** 2)
        elif kind == "lshelf":
            v = 0.55 - 1.1 * (0.5 - 0.5 * math.tanh(u * 4.2))
        elif kind == "hshelf":
            v = 0.55 - 1.1 * (0.5 + 0.5 * math.tanh(u * 4.2))
        elif kind == "lcut":
            v = -0.5 + 1.5 * max(0.0, (-u - 0.05) / 0.95) ** 1.6
        elif kind == "hcut":
            v = -0.5 + 1.5 * max(0.0, (u - 0.05) / 0.95) ** 1.6
        elif kind == "notch":
            v = -0.5 + 1.5 * math.exp(-(u / 0.11) ** 2)
        elif kind == "bpass":
            v = -0.8 + 1.8 * min(1.0, abs(u) / 0.85) ** 1.4
        elif kind == "tilt":
            v = -0.65 * math.tanh(u * 3.2)
        elif kind == "ftilt":
            v = -0.65 * u
        else:   # all pass: a flat line with a small loop over it
            v = 0.45
        pts.append(((cxp + u * hw) * TEX, (cyp + v * hh) * TEX))
    d.line(pts, fill=255, width=int(1.4 * TEX), joint="curve")
    if kind == "apass":
        r = 2.6
        bx, by = cxp, cyp - 0.6
        d.arc([(bx - r) * TEX, (by - r) * TEX, (bx + r) * TEX, (by + r) * TEX], 160, 380, fill=255, width=int(1.0 * TEX))
        ax, ay = bx + r * math.cos(math.radians(20)), by + r * math.sin(math.radians(20))
        d.polygon([(ax * TEX, (ay - 1.6) * TEX), ((ax + 1.7) * TEX, (ay + 0.6) * TEX), ((ax - 1.4) * TEX, (ay + 0.9) * TEX)], fill=255)


# Grease pencil on the filter cassette: a wobbly ring round one scale number and a word beside it.
def grease():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    g = L.GREASE
    ctl = next(c for c in L.CONTROLS if c["id"] == g["control"])
    lab = next(l for l in ctl["labels"] if l["t"] == g["label"])
    r = L.KNOBS[ctl["kind"]]
    x, y = polar(ctl["x"], ctl["y"], r + L.LABEL_R, norm_to_deg(lab["n"]))
    pts = []
    for i in range(0, 400):
        t = i / 360.0 * 2 * math.pi           # a little more than once round, as a hand does
        wob = 1 + 0.08 * math.sin(3 * t + 1.3) + 0.04 * math.sin(7 * t)
        pts.append(((x + 9.5 * wob * math.cos(t - 0.6)) * TEX, (y + 7.0 * wob * math.sin(t - 0.6) + 0.004 * i) * TEX))
    for i in range(len(pts) - 1):
        w = 1.25 + 0.45 * math.sin(i / 23.0)
        d.line([pts[i], pts[i + 1]], fill=230, width=max(1, int(w * TEX)))
    f = font("CaveatBrush-Regular.ttf", 14.5)
    nx, ny = x + 13, y - 13
    glyph = Image.new("L", (int(60 * TEX), int(30 * TEX)), 0)
    ImageDraw.Draw(glyph).text((2 * TEX, 15 * TEX), g["note"], font=f, fill=235, anchor="lm")
    glyph = glyph.rotate(9, resample=Image.BICUBIC, expand=False)
    im.paste(255, (int(nx * TEX), int((ny - 15) * TEX)), glyph)
    # Wax: breaks up where the pencil skipped over the hammertone.
    skip = blotch_noise(W, H, 2, 31, 0.5).point(lambda v: 0 if v > 186 else 255)
    im = ImageChops.multiply(im.filter(ImageFilter.GaussianBlur(0.35 * TEX / 4)), skip)
    im.save(os.path.join(OUT, "grease_ink.png"))


# A coffee ring on the top rail: darker at the rim where it dried, broken in places.
def coffee():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    c = L.COFFEE
    r = random.Random(77)
    for i in range(720):
        t = i / 720 * 2 * math.pi
        rr = c["r"] * (1 + 0.025 * math.sin(2 * t + 0.4) + 0.012 * math.sin(9 * t))
        x, y = (c["x"] + rr * math.cos(t)) * TEX, (c["y"] + rr * math.sin(t)) * TEX
        strength = 150 + 90 * math.sin(t * 1.3 + 2.0) ** 2
        if 2.2 < t < 2.9:
            strength *= 0.25
        d.ellipse([x - 1.1 * TEX, y - 1.1 * TEX, x + 1.1 * TEX, y + 1.1 * TEX], fill=int(strength))
    im = im.filter(ImageFilter.GaussianBlur(0.6 * TEX))
    # a faint wash inside the ring
    wash = Image.new("L", (W, H), 0)
    ImageDraw.Draw(wash).ellipse([(c["x"] - c["r"]) * TEX, (c["y"] - c["r"]) * TEX, (c["x"] + c["r"]) * TEX, (c["y"] + c["r"]) * TEX], fill=34)
    wash = ImageChops.multiply(wash.filter(ImageFilter.GaussianBlur(3 * TEX)), blotch_noise(W, H, 40, 78, 12).point(lambda v: int(80 + v * 0.7)))
    im = ImageChops.lighter(im, wash).point(lambda v: int(v * 0.42))   # a stain, not paint
    im.save(os.path.join(OUT, "coffee.png"))


# The inspection sticker: printed in red and black, initialled in blue ballpoint.
STICKER_TEX = 10


def sticker():
    s_ = L.STICKER
    S = STICKER_TEX
    w, h = s_["w"], s_["h"]
    im = Image.new("RGB", (int(w * S), int(h * S)), (238, 230, 205))
    d = ImageDraw.Draw(im)
    red, ink, blue = (178, 38, 30), (30, 26, 22), (38, 52, 120)
    d.rectangle([2.5 * S, 2.5 * S, (w - 2.5) * S, (h - 2.5) * S], outline=red, width=int(1.0 * S))
    d.rectangle([2.5 * S, 2.5 * S, (w - 2.5) * S, 14 * S], fill=red)
    f1 = font_px(LEGEND_BOLD, 9.0 * S)
    d.text((w * S / 2, 8.5 * S), s_["printed"][0], font=f1, fill=(244, 236, 214), anchor="mm")
    f2 = font_px(LEGEND, 7.6 * S)
    d.text((6 * S, 21 * S), s_["printed"][1], font=f2, fill=ink, anchor="lm")
    d.line([(6 * S, 38 * S), ((w - 6) * S, 38 * S)], fill=ink, width=int(0.4 * S))
    fh = font_px("CaveatBrush-Regular.ttf", 13 * S)
    d.text((8 * S, 32 * S), s_["hand"], font=fh, fill=blue, anchor="lm")
    # Age: yellowing toward the edges, a few fingermarks.
    age = Image.new("L", im.size, 0)
    ad = ImageDraw.Draw(age)
    for i in range(30):
        t = i / 30
        ad.rectangle([t * 14 * S, t * 10 * S, im.size[0] - t * 14 * S, im.size[1] - t * 10 * S], fill=int(255 * t))
    age = age.filter(ImageFilter.GaussianBlur(4 * S))
    im = Image.composite(im, Image.new("RGB", im.size, (205, 186, 140)), age.point(lambda v: int(140 + v * 0.45)))
    im = im.filter(ImageFilter.GaussianBlur(0.3 * S / 4))
    im.save(os.path.join(OUT, "sticker_color.png"))
    a = Image.new("L", im.size, 0)
    ad = ImageDraw.Draw(a)
    ad.rounded_rectangle([0.4 * S, 0.4 * S, (w - 0.4) * S, (h - 0.4) * S], radius=2.2 * S, fill=255)
    a.filter(ImageFilter.GaussianBlur(0.4 * S)).save(os.path.join(OUT, "sticker_alpha.png"))


# The maker's plate: engraved, the engraving filled black.
def plate_ink():
    p = L.SERIAL_PLATE
    S = STICKER_TEX
    w, h = p["w"], p["h"]
    im = Image.new("L", (int(w * S), int(h * S)), 0)
    d = ImageDraw.Draw(im)
    f1 = font_px("Jost.ttf", 9.6 * S, 560)
    f2 = font_px(LEGEND, 7.6 * S)
    d.text((w * S / 2, 11 * S), p["lines"][0], font=f1, fill=255, anchor="mm")
    d.text((w * S / 2, 23 * S), p["lines"][1], font=f2, fill=255, anchor="mm")
    d.rectangle([3 * S, 3 * S, (w - 3) * S, (h - 3) * S], outline=255, width=int(0.5 * S))
    im.save(os.path.join(OUT, "plate_ink.png"))


def nameplate_ink():
    p = L.NAMEPLATE
    S = STICKER_TEX
    w, h = p["w"], p["h"]
    im = Image.new("L", (int(w * S), int(h * S)), 0)
    d = ImageDraw.Draw(im)
    f1 = font_px("Jost.ttf", 23.0 * S, 560)
    # The name, widely spaced, as engravers set model names.
    text = p["name"]
    extra = 0.30 * f1.size
    total = sum(f1.getlength(ch) for ch in text) + extra * (len(text) - 1)
    x = (w * S - total) / 2
    for ch in text:
        d.text((x, 19.5 * S), ch, font=f1, fill=255, anchor="lm")
        x += f1.getlength(ch) + extra
    f2 = font_px(LEGEND, 6.9 * S)
    sub = p["sub"]
    extra2 = 0.16 * f2.size
    total2 = sum(f2.getlength(ch) for ch in sub) + extra2 * (len(sub) - 1)
    x = (w * S - total2) / 2
    for ch in sub:
        d.text((x, 36.5 * S), ch, font=f2, fill=255, anchor="lm")
        x += f2.getlength(ch) + extra2
    # A fine engraved border and two rules either side of the sub-line.
    d.rectangle([3.2 * S, 3.2 * S, (w - 3.2) * S, (h - 3.2) * S], outline=255, width=int(0.45 * S))
    sx0 = (w * S - total2) / 2
    for xa, xb in ((14 * S, sx0 - 6 * S), (sx0 + total2 + 6 * S, (w - 14) * S)):
        d.line([(xa, 36.5 * S), (xb, 36.5 * S)], fill=255, width=int(0.45 * S))
    im.save(os.path.join(OUT, "nameplate_ink.png"))


def font_px(name, px, weight=None):
    f = ImageFont.truetype(os.path.join(FONTS, name), int(round(px)))
    if weight is not None:
        try:
            f.set_variation_by_axes([weight])
        except Exception:
            pass
    return f


# Cream ink on the dark rails: rail legends.
def rail_legends():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    for lg in L.RAIL_LEGENDS:
        text_centered(d, (lg["x"], lg["y"]), lg["text"], font(LEGEND_BOLD, lg["size"]), spacing=0.14)
    im.save(os.path.join(OUT, "rail_ink.png"))


# ---------------------------------------------------------------------------
# The worn brand: screen-printed cream on the top rail, faded and rubbed.
def blotch_noise(w, h, cell, seed, blur):
    r = random.Random(seed)
    small = Image.new("L", (max(2, w // cell), max(2, h // cell)))
    small.putdata([r.randint(0, 255) for _ in range(small.size[0] * small.size[1])])
    return small.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(blur))


def brand():
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    b = L.BRAND
    f = font("Jost.ttf", b["size"], weight=520)
    # Left-aligned with wide letter spacing, as period screen printing was.
    x = b["x"] * TEX
    y = b["y"] * TEX
    extra = 0.19 * f.size
    for ch in b["text"]:
        d.text((x, y), ch, font=f, fill=255, anchor="lm")
        x += f.getlength(ch) + extra
    brand_right = x / TEX
    tp = L.TYPE_PLATE
    f2 = font("Jost.ttf", tp["size"], weight=480)
    x = tp["x"] * TEX
    for ch in tp["text"]:
        d.text((x, tp["y"] * TEX), ch, font=f2, fill=255, anchor="lm")
        x += f2.getlength(ch) + 0.28 * f2.size
    # A thin printed rule after the type, as on the old panels.
    d.line([(int(x + 5 * TEX), int(tp["y"] * TEX)), (int((brand_right - 4) * TEX), int(tp["y"] * TEX))], fill=255, width=int(0.5 * TEX))

    # Fading: an overall drop, broad blotches, fine pitting, and rubbing where hands go.
    fade = blotch_noise(W, H, 90, 3, 30).point(lambda v: int(96 + v * 0.46))
    pits = blotch_noise(W, H, 3, 4, 0.6).point(lambda v: 0 if v > 150 else 255)
    rub = Image.new("L", (W, H), 255)
    rd = ImageDraw.Draw(rub)
    for _ in range(26):   # horizontal rub marks across the lettering
        yy = (b["y"] + rng.uniform(-9, 9)) * TEX
        xx = (b["x"] + rng.uniform(0, 330)) * TEX
        ln = rng.uniform(20, 90) * TEX
        rd.line([(xx, yy), (xx + ln, yy + rng.uniform(-3, 3) * TEX)], fill=rng.randint(60, 170), width=int(rng.uniform(0.5, 2.2) * TEX))
    rub = rub.filter(ImageFilter.GaussianBlur(2.5))
    out = ImageChops.multiply(im, fade)
    out = ImageChops.multiply(out, pits)
    out = ImageChops.multiply(out, rub)
    out = out.filter(ImageFilter.GaussianBlur(0.7))
    out.save(os.path.join(OUT, "brand_ink.png"))


# ---------------------------------------------------------------------------
# Meter faces. VU: needle deflection is proportional to voltage, full scale +3 VU.
METER_INSET = 8.0          # points from the meter's outer edge to its window
METER_TEX = 8              # face texture pixels per point


def meter_geometry(m):
    fw, fh = m["w"] - 2 * METER_INSET, m["h"] - 2 * METER_INSET
    # The needle pivots below the window; the scale arc sits in the window's upper half.
    pivot = (fw / 2, fh * 1.02)
    radius = fh * 0.72
    return fw, fh, pivot, radius


VU_SPAN = 42.0             # degrees either side of vertical at 0 % and full scale


def vu_angle(vu):
    f = 10 ** (vu / 20) / 10 ** (3 / 20)
    return -VU_SPAN + 2 * VU_SPAN * f


GR_SPAN = 42.0
LUFS_LO, LUFS_HI = -24.0, -6.0
PLR_LO, PLR_HI = 3.0, 18.0


def lufs_angle(v):
    return -VU_SPAN + 2 * VU_SPAN * (min(max(v, LUFS_LO), LUFS_HI) - LUFS_LO) / (LUFS_HI - LUFS_LO)


def plr_angle(v):
    return -VU_SPAN + 2 * VU_SPAN * (min(max(v, PLR_LO), PLR_HI) - PLR_LO) / (PLR_HI - PLR_LO)


GR_EXP = 0.7               # scale law: room for the first few dB without crowding
MOVE_SPAN = 42.0


def move_angle(db):        # centre zero, linear in dB, +-12 dB full scale
    return MOVE_SPAN * max(-12.0, min(12.0, db)) / 12.0


def gr_angle(gr):          # gr >= 0 dB of reduction; rests at the right
    t = (min(max(gr, 0.0), 20.0) / 20.0) ** GR_EXP
    return GR_SPAN - 2 * GR_SPAN * t


def meter_face(kind, w, h, label, path):
    m = {"w": w, "h": h}
    fw, fh, pivot, radius = meter_geometry(m)
    S = METER_TEX
    im = Image.new("RGB", (int(fw * S), int(fh * S)), (236, 226, 196))
    # Aged paper: a faint warm vignette and blotches.
    vig = Image.new("L", im.size, 0)
    vd = ImageDraw.Draw(vig)
    for i in range(40):
        t = i / 40
        vd.rectangle([t * 30, t * 24, im.size[0] - t * 30, im.size[1] - t * 24], fill=int(255 * t))
    vig = vig.filter(ImageFilter.GaussianBlur(20))
    warm = Image.new("RGB", im.size, (205, 186, 140))
    im = Image.composite(im, warm, vig.point(lambda v: int(150 + v * 0.41)))
    bl = blotch_noise(im.size[0], im.size[1], 30, 9 if kind == "vu" else 10, 14)
    im = Image.composite(im, Image.new("RGB", im.size, (222, 206, 166)), bl.point(lambda v: 255 if v > 110 else int(200 + v / 2)))

    d = ImageDraw.Draw(im)
    ink, red = (24, 22, 20), (176, 36, 28)
    px, py = pivot[0] * S, pivot[1] * S
    R = radius * S

    def pt(r, deg):
        a = math.radians(deg)
        return px + r * math.sin(a), py - r * math.cos(a)

    def arc(r, a0, a1, col, width):
        # PIL measures from 3 o'clock; ours from 12. Width grows inward from r.
        rr = r + width / 2
        d.arc([px - rr, py - rr, px + rr, py + rr], a0 - 90, a1 - 90, fill=col, width=width)

    f_big = ImageFont.truetype(os.path.join(FONTS, LEGEND), int(8.2 * S))
    f_sm = ImageFont.truetype(os.path.join(FONTS, LEGEND_MED), int(5.6 * S))
    f_unit = ImageFont.truetype(os.path.join(FONTS, "Jost.ttf"), int(10 * S))
    try:
        f_unit.set_variation_by_axes([560])
    except Exception:
        pass

    if kind == "vu":
        arc(R, vu_angle(-20), vu_angle(0), ink, int(0.8 * S))
        arc(R, vu_angle(0), vu_angle(3), red, int(0.8 * S))
        arc(R + 2.0 * S, vu_angle(0), vu_angle(3), red, int(2.6 * S))  # the red band
        marks = [-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3]
        minor = [-15, -6, -4, -0.5, 0.5, 1.5, 2.5]
        for v in marks:
            a = vu_angle(v)
            col = red if v > 0 else ink
            d.line([pt(R, a), pt(R + 6.0 * S, a)], fill=col, width=int(0.8 * S))
            if v == -2 and fw < 100:
                continue            # too tight to letter on a small meter; the tick stays
            txt = str(abs(v)) if v != 0 else "0"
            d.text(pt(R + 12.0 * S, a), txt, font=f_big, fill=col, anchor="mm")
        for v in minor:
            a = vu_angle(v)
            d.line([pt(R, a), pt(R + 3.5 * S, a)], fill=red if v > 0 else ink, width=int(0.6 * S))
        # The percentage scale, under the arc.
        for p in (0, 20, 40, 60, 80, 100):
            vv = 20 * math.log10(max(p, 0.5) / 100.0 * 10 ** (3 / 20)) if p > 0 else -60
            a = -VU_SPAN + 2 * VU_SPAN * (p / 100.0 / (10 ** (3 / 20)))
            if a < -VU_SPAN:
                continue
            d.line([pt(R - 1.0 * S, a), pt(R - 4.0 * S, a)], fill=ink, width=int(0.6 * S))
            if p in (0, 50, 100) or p == 20 or p == 60:
                d.text(pt(R - 8.5 * S, a), str(p), font=f_sm, fill=ink, anchor="mm")
        d.text((fw * S / 2, fh * S * 0.70), "VU", font=f_unit, fill=ink, anchor="mm")
        d.text((fw * S * 0.15, fh * S * 0.86), "–", font=f_big, fill=ink, anchor="mm")
        d.text((fw * S * 0.85, fh * S * 0.86), "+", font=f_big, fill=red, anchor="mm")
        if label:
            d.text((fw * S * 0.86, fh * S * 0.62), label, font=f_big, fill=ink, anchor="mm")
    elif kind in ("lufs", "plr"):
        ang = lufs_angle if kind == "lufs" else plr_angle
        lo, hi = (LUFS_LO, LUFS_HI) if kind == "lufs" else (PLR_LO, PLR_HI)
        hot0, hot1 = (-9.0, -6.0) if kind == "lufs" else (3.0, 6.0)
        arc(R, ang(lo), ang(hi), ink, int(0.8 * S))
        arc(R + 2.0 * S, ang(hot0), ang(hot1), red, int(2.6 * S))
        majors = [-24, -21, -18, -15, -12, -9, -6] if kind == "lufs" else [3, 6, 9, 12, 15, 18]
        v = lo
        while v <= hi + 1e-6:
            a = ang(v)
            big = round(v) in majors and abs(v - round(v)) < 1e-6
            col = red if (hot0 <= v <= hot1) else ink
            d.line([pt(R, a), pt(R + (6.0 if big else 3.5) * S, a)], fill=col, width=int((0.8 if big else 0.6) * S))
            if big:
                d.text(pt(R + 12.0 * S, a), str(abs(int(round(v)))), font=f_big, fill=col, anchor="mm")
            v += 1.0
        f_lab = ImageFont.truetype(os.path.join(FONTS, LEGEND), int(5.6 * S))
        d.text((fw * S / 2, fh * S * 0.66), "LUFS" if kind == "lufs" else "PLR", font=f_unit, fill=ink, anchor="mm")
        d.text((fw * S / 2, fh * S * 0.80), "SHORT-TERM LOUDNESS" if kind == "lufs" else "PUNCH  dB", font=f_lab, fill=ink, anchor="mm")
        d.text((fw * S * 0.13, fh * S * 0.86), "–", font=f_big, fill=ink, anchor="mm")
        d.text((fw * S * 0.87, fh * S * 0.86), "+", font=f_big, fill=red if kind == "lufs" else ink, anchor="mm")
    elif kind == "move":
        arc(R, move_angle(-12), move_angle(12), ink, int(0.8 * S))
        for v in (-12, -9, -6, -3, 0, 3, 6, 9, 12):
            a = move_angle(v)
            big = v in (-12, -6, 0, 6, 12)
            d.line([pt(R, a), pt(R + (6.0 if big else 3.5) * S, a)], fill=ink, width=int((0.9 if v == 0 else 0.7) * S))
            if big:
                d.text(pt(R + 11.5 * S, a), str(abs(v)), font=f_big, fill=ink, anchor="mm")
        f_lab = ImageFont.truetype(os.path.join(FONTS, LEGEND), int(6.4 * S))
        d.text((fw * S / 2, fh * S * 0.74), "BAND GAIN  dB", font=f_lab, fill=ink, anchor="mm")
        d.text((fw * S * 0.12, fh * S * 0.80), "–", font=f_big, fill=ink, anchor="mm")
        d.text((fw * S * 0.88, fh * S * 0.80), "+", font=f_big, fill=ink, anchor="mm")
    else:
        arc(R, gr_angle(20), gr_angle(0), ink, int(0.8 * S))
        for v in (0, 2, 4, 6, 10, 15, 20):
            a = gr_angle(v)
            d.line([pt(R, a), pt(R + 5.5 * S, a)], fill=ink, width=int(0.8 * S))
            d.text(pt(R + 11.5 * S, a), str(v), font=f_big, fill=ink, anchor="mm")
        for v in (1, 3, 5, 8, 12, 17.5):
            a = gr_angle(v)
            d.line([pt(R, a), pt(R + 3 * S, a)], fill=ink, width=int(0.6 * S))
        f_lab = ImageFont.truetype(os.path.join(FONTS, LEGEND), int(6.4 * S))
        d.text((fw * S / 2, fh * S * 0.74), "GAIN REDUCTION  dB", font=f_lab, fill=ink, anchor="mm")
    # A maker's line, small, at the bottom.
    f_mk = ImageFont.truetype(os.path.join(FONTS, LEGEND_MED), int(3.6 * S))
    d.text((fw * S / 2, fh * S * 0.93), "RUMPUS ROOM RECORDS", font=f_mk, fill=(90, 80, 62), anchor="mm")
    im = im.filter(ImageFilter.GaussianBlur(0.5))
    im.save(path)
    return {"fw": fw, "fh": fh, "pivot": pivot, "radius": radius}


# ---------------------------------------------------------------------------
# Masking tape: an alpha shape (torn ends, wavy edges), the crepe, and the handwriting.
TAPE_TEX = 8


def tape_shape(w, h, seed, path):
    r = random.Random(seed)
    S = TAPE_TEX
    pad = 6
    im = Image.new("L", (int((w + 2 * pad) * S), int((h + 2 * pad) * S)), 0)
    d = ImageDraw.Draw(im)
    pts = []
    # top edge, left to right, slightly wavy
    n = 40
    for i in range(n + 1):
        x = pad + w * i / n
        pts.append((x * S, (pad + r.uniform(-0.25, 0.25)) * S))
    # right end, torn: jagged zigzag going down
    m = 14
    for i in range(1, m):
        y = pad + h * i / m
        pts.append(((pad + w + r.uniform(-2.2, 1.4)) * S, y * S))
    for i in range(n, -1, -1):
        x = pad + w * i / n
        pts.append((x * S, (pad + h + r.uniform(-0.25, 0.25)) * S))
    for i in range(m - 1, 0, -1):
        y = pad + h * i / m
        pts.append(((pad + r.uniform(-1.4, 2.2)) * S, y * S))
    d.polygon(pts, fill=255)
    im = im.filter(ImageFilter.GaussianBlur(0.8))
    im.save(path)


def tape_crepe(w, h, seed, path):
    """Masking tape's crepe: fine ridges across the width, plus fibre noise."""
    r = random.Random(seed)
    S = TAPE_TEX
    pad = 6
    W2, H2 = int((w + 2 * pad) * S), int((h + 2 * pad) * S)
    im = Image.new("L", (W2, H2), 128)
    d = ImageDraw.Draw(im)
    x = 0.0
    while x < W2:
        x += r.uniform(0.35, 1.1) * S
        d.line([(x, 0), (x + r.uniform(-4, 4), H2)], fill=r.randint(100, 175), width=max(1, int(r.uniform(0.15, 0.4) * S)))
    im = im.filter(ImageFilter.GaussianBlur(0.9))
    fib = blotch_noise(W2, H2, 2, seed + 7, 0.5)
    im = ImageChops.blend(im, fib, 0.25)
    # Creases: a couple of soft diagonal folds.
    cr = Image.new("L", (W2, H2), 128)
    cd = ImageDraw.Draw(cr)
    for _ in range(3):
        x0 = r.uniform(0, W2)
        cd.line([(x0, 0), (x0 + r.uniform(-H2, H2) * 0.6, H2)], fill=r.choice([70, 190]), width=int(1.2 * S))
    cr = cr.filter(ImageFilter.GaussianBlur(3 * S / 4))
    im = ImageChops.blend(im, cr, 0.35)
    im.save(path)


def handwriting(w, h, text, seed, path, size_k=0.95):
    """'sam's desk' in marker: per-letter drift, wobble, uneven ink and a little bleed."""
    r = random.Random(seed)
    S = TAPE_TEX
    pad = 6
    W2, H2 = int((w + 2 * pad) * S), int((h + 2 * pad) * S)
    ink = Image.new("L", (W2, H2), 0)
    size = h * size_k
    f = ImageFont.truetype(os.path.join(FONTS, "CaveatBrush-Regular.ttf"), int(size * S))
    total = sum(f.getlength(ch) for ch in text) * 0.92
    x = (pad + w / 2) * S - total / 2
    base = (pad + h * 0.56) * S
    drift = 0.0
    for i, ch in enumerate(text):
        cw = f.getlength(ch)
        if ch == " ":
            x += cw * 0.75
            continue
        glyph = Image.new("L", (int(cw * 2.2) + 20, int(size * S * 1.8)), 0)
        gd = ImageDraw.Draw(glyph)
        gd.text((glyph.size[0] / 2, glyph.size[1] / 2), ch, font=f, fill=255, anchor="mm")
        sc = r.uniform(0.93, 1.06)
        glyph = glyph.resize((int(glyph.size[0] * sc), int(glyph.size[1] * sc)), Image.BICUBIC)
        glyph = glyph.rotate(r.uniform(-5, 5), resample=Image.BICUBIC)
        drift += r.uniform(-0.45, 0.45) * S
        gx = int(x + cw / 2 - glyph.size[0] / 2)
        gy = int(base - glyph.size[1] / 2 + drift - 0.05 * size * S * (i / len(text)))
        ink.paste(255, (gx, gy), glyph)
        x += cw * r.uniform(0.86, 0.96)
    # Marker ink: streaky along the stroke, darker where strokes overlap/start.
    streak = Image.new("L", (W2, H2), 0)
    sd = ImageDraw.Draw(streak)
    for _ in range(90):
        yy = r.uniform(0, H2)
        sd.line([(0, yy), (W2, yy + r.uniform(-6, 6))], fill=r.randint(0, 34), width=max(1, int(r.uniform(0.2, 0.6) * S)))
    streak = streak.filter(ImageFilter.GaussianBlur(2.0))
    dens = ImageChops.subtract(Image.new("L", (W2, H2), 255), streak)
    ink = ImageChops.multiply(ink.filter(ImageFilter.GaussianBlur(0.55 * S / 4)), dens)
    # Bleed: a faint halo into the paper fibres.
    bleed = ink.filter(ImageFilter.GaussianBlur(1.6 * S / 4)).point(lambda v: int(v * 0.35))
    ink = ImageChops.lighter(ink, bleed)
    ink.save(path)


def handwriting_words(w, h, words, seed, path, size_k=0.95):
    """Several words in marker, each centred at its own x (points from the tape's left edge)."""
    r = random.Random(seed)
    S = TAPE_TEX
    pad = 6
    W2, H2 = int((w + 2 * pad) * S), int((h + 2 * pad) * S)
    ink = Image.new("L", (W2, H2), 0)
    size = h * size_k
    f = ImageFont.truetype(os.path.join(FONTS, "CaveatBrush-Regular.ttf"), int(size * S))
    for text, xc in words:
        total = sum(f.getlength(ch) for ch in text) * 0.92
        x = (pad + xc) * S - total / 2
        base = (pad + h * 0.55) * S + r.uniform(-0.6, 0.6) * S
        drift = 0.0
        tilt = r.uniform(-3, 3)
        for ch in text:
            cw = f.getlength(ch)
            glyph = Image.new("L", (int(cw * 2.2) + 20, int(size * S * 1.8)), 0)
            ImageDraw.Draw(glyph).text((glyph.size[0] / 2, glyph.size[1] / 2), ch, font=f, fill=255, anchor="mm")
            sc = r.uniform(0.93, 1.06)
            glyph = glyph.resize((int(glyph.size[0] * sc), int(glyph.size[1] * sc)), Image.BICUBIC)
            glyph = glyph.rotate(r.uniform(-5, 5) + tilt, resample=Image.BICUBIC)
            drift += r.uniform(-0.35, 0.35) * S
            ink.paste(255, (int(x + cw / 2 - glyph.size[0] / 2), int(base - glyph.size[1] / 2 + drift)), glyph)
            x += cw * r.uniform(0.86, 0.96)
    streak = Image.new("L", (W2, H2), 0)
    sd = ImageDraw.Draw(streak)
    for _ in range(60):
        yy = r.uniform(0, H2)
        sd.line([(0, yy), (W2, yy + r.uniform(-6, 6))], fill=r.randint(0, 34), width=max(1, int(r.uniform(0.2, 0.6) * S)))
    streak = streak.filter(ImageFilter.GaussianBlur(2.0))
    dens = ImageChops.subtract(Image.new("L", (W2, H2), 255), streak)
    ink = ImageChops.multiply(ink.filter(ImageFilter.GaussianBlur(0.55 * S / 4)), dens)
    bleed = ink.filter(ImageFilter.GaussianBlur(1.6 * S / 4)).point(lambda v: int(v * 0.35))
    ImageChops.lighter(ink, bleed).save(path)


# ---------------------------------------------------------------------------
# The SPACE tray: cream legends on the dark tray, a shadow where the desk overhangs it, its plate.
def tray_textures():
    TW, TH = L.TRAY_W * TEX, L.TRAY_H * TEX
    im = Image.new("L", (TW, TH), 0)
    d = ImageDraw.Draw(im)
    draw_controls(d, L.TRAY_CONTROLS)
    tb = L.TRAY_TABLE
    f_h = font(LEGEND_BOLD, L.TYPE["legend"])
    f_r = font(LEGEND_MED, L.TYPE["legend"])
    for i, row in enumerate(tb["rows"]):
        yy = (tb["y"] + i * tb["step"]) * TEX
        for cxp, txt in zip(tb["cols"], row):
            d.text((cxp * TEX, yy), txt, font=f_h if (i == 0 or cxp == tb["cols"][0]) else f_r, fill=255, anchor="lm")
    x0, x1 = tb["cols"][0] - 4, L.TRAY_W - 22
    yl = (tb["y"] + 0.5 * tb["step"]) * TEX
    d.line([(x0 * TEX, yl), (x1 * TEX, yl)], fill=255, width=int(0.6 * TEX))
    for cxp in tb["cols"][1:]:
        d.line([((cxp - 5) * TEX, (tb["y"] - 6) * TEX), ((cxp - 5) * TEX, (tb["y"] + 6.5 * tb["step"]) * TEX)], fill=255, width=int(0.5 * TEX))
    im.save(os.path.join(OUT, "tray_ink.png"))
    # The desk's overhang: dark at the top edge, fading in 18 points.
    sh = Image.new("L", (TW, TH), 0)
    sd = ImageDraw.Draw(sh)
    for yy in range(int(18 * TEX)):
        t = 1 - yy / (18 * TEX)
        sd.line([(0, yy), (TW, yy)], fill=int(235 * t ** 1.8))
    sh.save(os.path.join(OUT, "tray_shadow.png"))
    p = L.TRAY_PLATE
    S = STICKER_TEX
    w, h = p["w"], p["h"]
    pi = Image.new("L", (int(w * S), int(h * S)), 0)
    pd = ImageDraw.Draw(pi)
    f1 = font_px("Jost.ttf", 17.0 * S, 560)
    text = p["name"]
    extra = 0.28 * f1.size
    total = sum(f1.getlength(ch) for ch in text) + extra * (len(text) - 1)
    x = (w * S - total) / 2
    for ch in text:
        pd.text((x, 16.5 * S), ch, font=f1, fill=255, anchor="lm")
        x += f1.getlength(ch) + extra
    f2 = font_px(LEGEND, 6.4 * S)
    pd.text((w * S / 2, 31.5 * S), p["sub"], font=f2, fill=255, anchor="mm")
    pd.rectangle([3.0 * S, 3.0 * S, (w - 3.0) * S, (h - 3.0) * S], outline=255, width=int(0.45 * S))
    pi.save(os.path.join(OUT, "tray_plate_ink.png"))


# ---------------------------------------------------------------------------
# Wear: polished rings where fingers turn the knobs, scuffs, and dirt in corners.
def wear():
    im = Image.new("L", (W, H), 0)          # 255 = worn (polished, a touch lighter)
    d = ImageDraw.Draw(im)
    for ctl in L.CONTROLS:
        if ctl["type"] != "knob":
            continue
        r = L.KNOBS[ctl["kind"]]
        x, y = ctl["x"] * TEX, ctl["y"] * TEX
        for _ in range(int(r * 1.4)):
            a0 = rng.uniform(0, 360)
            rr = (r + rng.uniform(0.5, 6)) * TEX
            d.arc([x - rr, y - rr, x + rr, y + rr], a0, a0 + rng.uniform(15, 80), fill=rng.randint(40, 140), width=max(1, int(rng.uniform(0.2, 0.6) * TEX)))
    for _ in range(140):   # random scuffs
        x, y = rng.uniform(0, W), rng.uniform(L.CASS_Y[0] * TEX, L.CASS_Y[1] * TEX)
        ln = rng.uniform(4, 28) * TEX
        a = rng.uniform(-0.6, 0.6) + (0 if rng.random() < 0.6 else math.pi / 2)
        d.line([(x, y), (x + ln * math.cos(a), y + ln * math.sin(a))], fill=rng.randint(30, 110), width=max(1, int(rng.uniform(0.2, 0.5) * TEX)))
    im = im.filter(ImageFilter.GaussianBlur(0.8))
    im.save(os.path.join(OUT, "wear.png"))
    # Grime: darker toward the bottom of each cassette and in the corners.
    g = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(g)
    for c in L.CASSETTES:
        x0, x1 = c["x0"] * TEX, c["x1"] * TEX
        for i in range(30):
            t = i / 30
            gd.rectangle([x0 + t * 14 * TEX, L.CASS_Y[0] * TEX + t * 12 * TEX, x1 - t * 14 * TEX, L.CASS_Y[1] * TEX - t * 18 * TEX], outline=int(160 * (1 - t) ** 2), width=int(0.6 * TEX))
    g = g.filter(ImageFilter.GaussianBlur(6 * TEX))
    noise = blotch_noise(W, H, 60, 21, 20)
    g = ImageChops.multiply(g, noise.point(lambda v: int(120 + v / 2)))
    g.save(os.path.join(OUT, "grime.png"))


# ---------------------------------------------------------------------------
# The tube's face: an etched graticule (lit faintly from the edge) and the frequency scale.
FACE_TEX = 6


def crt_face():
    F, G = L.FACE, L.GRID
    S = FACE_TEX
    fw, fh = F["x1"] - F["x0"], F["y1"] - F["y0"]
    im = Image.new("L", (int(fw * S), int(fh * S)), 0)
    d = ImageDraw.Draw(im)
    left, right = (G["x0"] - F["x0"]) * S, (G["x1"] - F["x0"]) * S
    top, bot = (G["y0"] - F["y0"]) * S, (G["y1"] - F["y0"]) * S
    for i in range(G["nx"] + 1):
        x = left + (right - left) * i / G["nx"]
        d.line([(x, top), (x, bot)], fill=74, width=max(1, int(0.45 * S)))
    for j in range(G["ny"] + 1):
        y = top + (bot - top) * j / G["ny"]
        d.line([(left, y), (right, y)], fill=74, width=max(1, int(0.45 * S)))
    # Fine ticks along the centre lines, five to a division, as on a scope graticule.
    xc, yc = (left + right) / 2, (top + bot) / 2
    for k in range(G["nx"] * 5 + 1):
        x = left + (right - left) * k / (G["nx"] * 5)
        d.line([(x, yc - 1.6 * S), (x, yc + 1.6 * S)], fill=96, width=max(1, int(0.3 * S)))
    for k in range(G["ny"] * 5 + 1):
        y = top + (bot - top) * k / (G["ny"] * 5)
        d.line([(xc - 1.6 * S, y), (xc + 1.6 * S, y)], fill=96, width=max(1, int(0.3 * S)))
    d.rectangle([left, top, right, bot], outline=120, width=max(1, int(0.6 * S)))
    im = im.filter(ImageFilter.GaussianBlur(0.35 * S / 2))
    im.save(os.path.join(OUT, "crt_graticule.png"))


def logn(lo, hi, v):
    return math.log(v / lo) / math.log(hi / lo)


# The selected band's number on two drum wheels behind a small window.
DRUM_TEX = 12


def drum_face(spec, blank=False):
    S = DRUM_TEX
    w, h = spec["w"] - 6, spec["h"] - 6
    im = Image.new("RGB", (int(w * S), int(h * S)), (16, 15, 14))
    f = ImageFont.truetype(os.path.join(FONTS, LEGEND_BOLD), int(17 * S))
    text = spec["text"]
    n = len(text)
    wheel_w = (w - 1.0) / n - 1.0
    for k, ch in enumerate(text):
        x0 = (1.0 + k * (wheel_w + 1.0)) * S
        wheel = Image.new("RGB", (int(wheel_w * S), int(h * S)), (22, 21, 19))
        wd = ImageDraw.Draw(wheel)
        if not blank:
            if ch.isdigit():
                dgt = int(ch)
                for off in (-1, 0, 1):    # the neighbours peep in above and below
                    wd.text((wheel.size[0] / 2, wheel.size[1] / 2 + off * 19 * S), str((dgt + off) % 10), font=f,
                            fill=(236, 230, 212), anchor="mm")
            else:
                wd.text((wheel.size[0] / 2, wheel.size[1] / 2), ch, font=f, fill=(236, 230, 212), anchor="mm")
        shade = Image.new("L", wheel.size, 0)
        sd = ImageDraw.Draw(shade)
        for yy in range(wheel.size[1]):
            t = (yy / wheel.size[1]) * 2 - 1
            sd.line([(0, yy), (wheel.size[0], yy)], fill=int(255 * max(0.0, math.cos(t * 1.35)) ** 1.6))
        wheel = Image.composite(wheel, Image.new("RGB", wheel.size, (4, 4, 4)), shade)
        im.paste(wheel, (int(x0), 0))
    im = im.filter(ImageFilter.GaussianBlur(0.6))
    im.save(os.path.join(OUT, f"drum_{spec['id']}{'_blank' if blank else ''}.png"))


if __name__ == "__main__":
    legends()
    rail_legends()
    brand()
    grease()
    coffee()
    sticker()
    plate_ink()
    nameplate_ink()
    crt_face()
    for r in L.READOUTS:
        drum_face(r)
        drum_face(r, blank=True)
    geo = {}
    ml = L.control("m_loud")
    geo["lufs"] = meter_face("lufs", ml["w"], ml["h"], None, os.path.join(OUT, "face_lufs.png"))
    mp = L.control("m_punch")
    geo["plr"] = meter_face("plr", mp["w"], mp["h"], None, os.path.join(OUT, "face_plr.png"))
    pt_ = L.PRESET_TAPE
    tape_shape(pt_["w"], pt_["h"], 8, os.path.join(OUT, "tape_preset_alpha.png"))
    tape_crepe(pt_["w"], pt_["h"], 9, os.path.join(OUT, "tape_preset_crepe.png"))
    handwriting(pt_["w"], pt_["h"], L.PRESET_TEXT, 12, os.path.join(OUT, "tape_preset_ink.png"), size_k=0.7)
    st_ = L.SECT_TAPE
    tape_shape(st_["w"], st_["h"], 21, os.path.join(OUT, "tape_sect_alpha.png"))
    tape_crepe(st_["w"], st_["h"], 22, os.path.join(OUT, "tape_sect_crepe.png"))
    words = [(nm, L.control(f"sec_{i + 1}")["x"] - (st_["x"] - st_["w"] / 2)) for i, nm in enumerate(L.SECT_NAMES)]
    handwriting_words(st_["w"], st_["h"], words, 23, os.path.join(OUT, "tape_sect_ink.png"), size_k=0.92)
    tray_textures()
    wear()
    import json
    with open(os.path.join(OUT, "meter_geometry.json"), "w") as fh:
        json.dump({"inset": METER_INSET, **geo}, fh, indent=1)
    print("textures written to", OUT)
