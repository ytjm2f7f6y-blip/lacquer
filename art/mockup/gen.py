"""Builds project/Main.dc.html for the Lacquer front-panel canvas from the art layout.

    python3 gen.py blobs.json     (panel, tray and each lit crop -> its /_blob/ url)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import layout as L  # noqa: E402

BLOBS = json.load(open(sys.argv[1])) if len(sys.argv) > 1 else {}


def blob(name):
    return BLOBS.get(name, "/_blob/" + name.upper())


def px(x, y, w, h):
    return "left:%gpx;top:%gpx;width:%gpx;height:%gpx" % (round(x, 2), round(y, 2), round(w, 2), round(h, 2))


def knob_box(c):
    r = L.KNOBS[c["kind"]]
    lr = c.get("label_r") or (r + L.LABEL_R)
    hw = max(r + 21, lr + 6) if c["kind"] == "sel" else r + 21
    top = c["y"] - (lr + 10)
    bot = c["y"] + r + L.NAME_GAP + (L.SUB_GAP if c.get("sub") else 0) + 8
    return c["x"] - hw, top, 2 * hw, bot - top


SKIP = ("preset_prev", "preset_next", "ab_a", "ab_b", "sec_mark")


def zones_for(controls):
    out = []
    for c in controls:
        t = c["type"]
        if t == "knob":
            out.append((c["id"],) + knob_box(c))
        elif t == "toggle":
            out.append((c["id"], c["x"] - 30, c["y"] - 34, 60, 84))
        elif t == "button" and not c.get("key") and c["id"] not in SKIP:
            out.append((c["id"], c["x"] - c["w"] / 2 - 4, c["y"] - c["h"] / 2 - 4, c["w"] + 8, c["h"] + 8))
        elif t == "lamp":
            if c["id"].endswith("_idea"):
                out.append((c["id"], c["x"] - 22, c["y"] - 10, 44, 30))
            elif c["id"] == "power":
                out.append((c["id"], c["x"] - 16, c["y"] - 24, 32, 36))
            elif c["id"] == "space_on":
                out.append((c["id"], c["x"] - 10, c["y"] - 10, 20, 20))
            else:
                out.append((c["id"], c["x"] - 44, c["y"] - 10, 56, 20))
        elif t == "meter":
            out.append((c["id"], c["x"] - c["w"] / 2 - 3, c["y"] - c["h"] / 2 - 3, c["w"] + 6, c["h"] + 6))
    return out


zones = zones_for(L.CONTROLS)
for r in L.READOUTS:
    zones.append((r["id"], r["x"] - r["w"] / 2 - 10, r["y"] - r["h"] / 2 - 4, r["w"] + 20, r["h"] + 34))
st = L.SECT_TAPE
zones += [
    ("preset", 276, 668, 508, 40),
    ("compare", 828, 668, 70, 40),
    ("sect_tape", st["x"] - st["w"] / 2, st["y"] - st["h"] / 2 - 2, st["w"], st["h"] + 4),
]


def hover_attrs(zid):
    return 'onMouseEnter="{{ hov.%s }}" onMouseLeave="{{ leave }}" onFocus="{{ hov.%s }}" onBlur="{{ leave }}"' % (zid, zid)


btns = ['<button class="hs" type="button" aria-label="%s" style="%s" %s></button>' % (z[0], px(*z[1:]), hover_attrs(z[0]))
        for z in zones]

# Lit crops (each shows while its key is the selected one) and the clickable keys.
lits, keys = [], []
PAGE_OF = {"key_bal": "bal", "key_loud": "loud", "key_moves": "moves", "key_checks": "checks", "key_sect": "sect"}
for kid, text in L.SCREEN_KEYS:
    x0, y0, w, h = L.lit_rect(L.control(kid))
    pg = PAGE_OF[kid]
    lits.append('<sc-if value="{{ pg.%s }}" hint-placeholder-val="{{ %s }}"><img class="lit" src="%s" alt="" style="%s"></sc-if>'
                % (pg, "true" if pg == "bal" else "false", blob(kid), px(x0, y0, w, h)))
    c = L.control(kid)
    keys.append('<button class="hs key" type="button" aria-label="screen: %s" style="%s" onClick="{{ go.%s }}" %s></button>'
                % (text.lower(), px(c["x"] - c["w"] / 2 - 4, c["y"] - c["h"] / 2 - 4, c["w"] + 8, c["h"] + 8), pg, hover_attrs(kid)))
for kid in L.SECT_KEYS:
    x0, y0, w, h = L.lit_rect(L.control(kid))
    sk = kid[4:]   # song, 1..5
    lits.append('<sc-if value="{{ sec.on_%s }}" hint-placeholder-val="{{ %s }}"><img class="lit" src="%s" alt="" style="%s"></sc-if>'
                % (sk, "true" if sk == "song" else "false", blob("key_" + kid), px(x0, y0, w, h)))
    c = L.control(kid)
    label = "song" if sk == "song" else "section " + sk + ", " + L.SECT_NAMES[int(sk) - 1]
    keys.append('<button class="hs key" type="button" aria-label="%s" aria-pressed="{{ sec.pressed_%s }}" style="%s" onClick="{{ sec.pick_%s }}" %s></button>'
                % (label, sk, px(c["x"] - c["w"] / 2 - 4, c["y"] - c["h"] / 2 - 4, c["w"] + 8, c["h"] + 8), sk, hover_attrs(kid)))
m = L.control("sec_mark")
x0, y0, w, h = L.lit_rect(m)
lits.append('<sc-if value="{{ sec.marking }}" hint-placeholder-val="{{ false }}"><img class="lit" src="%s" alt="" style="%s"></sc-if>'
            % (blob("mark"), px(x0, y0, w, h)))
keys.append('<button class="hs key" type="button" aria-label="mark sections" aria-pressed="{{ sec.markPressed }}" style="%s" onClick="{{ sec.toggleMark }}" %s></button>'
            % (px(m["x"] - m["w"] / 2 - 4, m["y"] - m["h"] / 2 - 4, m["w"] + 8, m["h"] + 8), hover_attrs("sec_mark")))
la = L.SPACE_LATCH
keys.append('<button class="hs key" type="button" aria-label="space tray" aria-expanded="{{ tray.expanded }}" style="%s" onClick="{{ tray.toggle }}" %s></button>'
            % (px(la["x"] - 14, la["y"] - 14, 28, 28), hover_attrs("latch")))

# The tray's own hotspots, laid over its picture.
tz = zones_for(L.TRAY_CONTROLS)
tb = L.TRAY_TABLE
tp = L.TRAY_PLATE
tz += [("tray_table", tb["cols"][0] - 8, tb["y"] - 10, L.TRAY_W - 16 - (tb["cols"][0] - 8), 7 * tb["step"] + 6),
       ("tray_plate", tp["x"] - tp["w"] / 2 - 4, tp["y"] - tp["h"] / 2 - 4, tp["w"] + 8, tp["h"] + 8)]
tray_btns = ['<button class="hs" type="button" aria-label="%s" style="%s" %s></button>' % (z[0], px(*z[1:]), hover_attrs(z[0]))
             for z in tz]

F = L.FACE
SX, SY = F["x0"] - 2, F["y0"] - 2
SW, SH = F["x1"] - F["x0"] + 4, F["y1"] - F["y0"] + 4

HTML = open(os.path.join(HERE, "template.html")).read()
subs = {
    "@@PANEL@@": blob("panel"), "@@TRAY@@": blob("tray"), "@@BUTTONS@@": "\n".join(btns), "@@LITS@@": "\n".join(lits),
    "@@KEYS@@": "\n".join(keys), "@@TRAYBTNS@@": "\n".join(tray_btns),
    "@@SX@@": "%g" % SX, "@@SY@@": "%g" % SY, "@@SW@@": "%g" % SW, "@@SH@@": "%g" % SH,
    "@@TRAYH@@": "%g" % L.TRAY_H, "@@FULLH@@": "%g" % (L.H + L.TRAY_H + 80),
}
for a, b in subs.items():
    HTML = HTML.replace(a, b)
assert "@@" not in HTML, HTML[HTML.index("@@") - 40: HTML.index("@@") + 40]
os.makedirs(os.path.join(HERE, "project"), exist_ok=True)
open(os.path.join(HERE, "project", "Main.dc.html"), "w").write(HTML)
print("zones", len(zones), "tray zones", len(tz), "screen", SX, SY, SW, SH)
