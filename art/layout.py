"""Lacquer: the faceplate layout, in points at 100% (the art is rendered at 2x).

Forked from Kontur, same conventions: one source for the Blender render, the
printed legends and the mockup's hover zones. Angles: 0 = straight up, clockwise
positive; continuous knobs sweep -150..+150 degrees.

`set` on a control is where the mockup render puts it (a 0..1 position for
continuous knobs, a step index for selectors, "up"/"down" for toggles,
True/False for lit buttons and lamps).
"""
import math

W, H = 1200, 720
SCALE = 2
RAIL_TOP = (0, 64)
CASS_Y = (64, 656)
RAIL_BOT = (656, 720)
RAIL_MID = (RAIL_BOT[0] + RAIL_BOT[1]) / 2

SWEEP = 150.0
STRIP_FRAMES = 121

KNOBS = {"large": 30, "med": 24, "small": 18, "sel": 23}

TYPE = {
    "title": 12.5,
    "name": 9.8,
    "num": 8.4,
    "sub": 7.8,
    "toggle": 9.0,
    "legend": 8.9,
}
LABEL_R = 15.5
NAME_GAP = 23.0
SUB_GAP = 10.0

# The assistant on the left, six processing cassettes in signal order, the output on the right.
CASSETTES = [
    {"id": "asst", "title": "ASSISTANT", "x0": 24, "x1": 456},
    {"id": "clean", "title": "CLEAN", "x0": 456, "x1": 536, "strip": True},
    {"id": "tone", "title": "TONE", "x0": 536, "x1": 616, "strip": True},
    {"id": "dyn", "title": "DYNAMICS", "x0": 616, "x1": 696, "strip": True},
    {"id": "col", "title": "COLOUR", "x0": 696, "x1": 776, "strip": True},
    {"id": "st", "title": "STEREO", "x0": 776, "x1": 856, "strip": True},
    {"id": "loud", "title": "LOUDNESS", "x0": 856, "x1": 936, "strip": True},
    {"id": "out", "title": "OUTPUT", "x0": 936, "x1": 1176},
]
IN_Y = 636

# The cathode-ray tube: a phenolic bezel round a dark green face.
CRT = {"x": 240, "y": 230, "w": 404, "h": 240}
CRT_INSET = 14.0
FACE = {"x0": CRT["x"] - CRT["w"] / 2 + CRT_INSET, "y0": CRT["y"] - CRT["h"] / 2 + CRT_INSET,
        "x1": CRT["x"] + CRT["w"] / 2 - CRT_INSET, "y1": CRT["y"] + CRT["h"] / 2 - CRT_INSET}
# The etched graticule: a plain scope grid; each screen page draws its own scale over it.
GRID = {"x0": FACE["x0"] + 6, "y0": FACE["y0"] + 6, "x1": FACE["x1"] - 6, "y1": FACE["y1"] - 6, "nx": 10, "ny": 6}

POINTERS = {
    "a_amount": "brick",
    "t_air": "slate", "d_glue": "sage", "c_drive": "ochre",
    "l_target": "terracotta", "o_monitor": "slate",
}
POINTER_COLOURS = {
    "brick": (0.84, 0.40, 0.33), "slate": (0.56, 0.68, 0.84), "sage": (0.62, 0.78, 0.58),
    "ochre": (0.93, 0.77, 0.44), "terracotta": (0.93, 0.58, 0.40),
}


def cx(i):
    c = CASSETTES[i]
    return (c["x0"] + c["x1"]) / 2


def cas(cid):
    return next(i for i, c in enumerate(CASSETTES) if c["id"] == cid)


def lin(lo, hi, v):
    return (v - lo) / (hi - lo)


def logn(lo, hi, v):
    return math.log(v / lo) / math.log(hi / lo)


def ticks_lin(lo, hi, step):
    n = int(round((hi - lo) / step))
    return [i / n for i in range(n + 1)]


def labels(pairs):
    return [{"n": n, "t": t} for n, t in pairs]


def steps(n, spacing=30.0):
    return [(-(n - 1) / 2 + i) * spacing for i in range(n)]


CONTROLS = []


def knob(pid, kind, x, y, name, lab, ticks=None, sub=None, set_=0.5):
    CONTROLS.append({"id": pid, "type": "knob", "kind": kind, "x": x, "y": y, "name": name, "sub": sub,
                     "labels": lab, "ticks": ticks if ticks is not None else [l["n"] for l in lab],
                     "steps": None, "set": set_})


def selector(pid, x, y, name, texts, sub=None, spacing=30.0, set_=0, label_r=None):
    ang = steps(len(texts), spacing)
    CONTROLS.append({"id": pid, "type": "knob", "kind": "sel", "x": x, "y": y, "name": name, "sub": sub,
                     "labels": [{"a": a, "t": t} for a, t in zip(ang, texts)], "ticks": [], "steps": ang, "set": set_,
                     "label_r": label_r})


def toggle(pid, x, y, up, down, name=None, set_="down"):
    CONTROLS.append({"id": pid, "type": "toggle", "x": x, "y": y, "up": up, "down": down, "name": name, "set": set_})


def button(pid, x, y, text="IN", w=36, h=20, set_=False, **kw):
    CONTROLS.append({"id": pid, "type": "button", "x": x, "y": y, "w": w, "h": h, "text": text, "set": set_, **kw})


def lamp(pid, x, y, r, color, set_=False, legend=None):
    CONTROLS.append({"id": pid, "type": "lamp", "x": x, "y": y, "r": r, "color": color, "set": set_, "legend": legend})


TEN = ticks_lin(0, 10, 1)

# ---- ASSISTANT: the tube, the screen keys, the target and how much of the advice to take
c = cx(cas("asst"))
CONTROLS.append({"id": "crt", "type": "crt", **CRT})

# Four screen keys. The green one is the page on the tube (the mockup lights it on top of the photo).
KEY_Y = 378
SCREEN_KEYS = [("key_bal", "BALANCE"), ("key_loud", "LOUDNESS"), ("key_moves", "MOVES"), ("key_checks", "CHECKS"),
               ("key_sect", "SECTIONS"), ("key_detail", "DETAIL")]
KEY_W, KEY_H, KEY_PITCH = 57, 22, 63.0
for i, (kid, text) in enumerate(SCREEN_KEYS):
    button(kid, c + (i - (len(SCREEN_KEYS) - 1) / 2) * KEY_PITCH, KEY_Y, text, w=KEY_W, h=KEY_H, set_=False, key=True, selected=False)
KEYS_NAME_Y = 403

ROW_A = 492
selector("a_target", 70, ROW_A, "TARGET", ["GENRE", "REFS", "MINE"], spacing=40.0, set_=0)
selector("a_dest", 168, ROW_A, "DESTINATION", ["STREAM", "CD", "VINYL", "CLUB"], spacing=40.0, set_=0, label_r=42.0)
knob("a_amount", "large", 264, ROW_A, "AMOUNT", labels([(0, "0"), (0.5, "50"), (1, "100")]), ticks=TEN, sub="%", set_=0.7)
toggle("a_learn", 346, ROW_A - 4, "ON", "OFF", name="LEARN", set_="up")
button("a_listen", 412, ROW_A - 20, "LISTEN", w=54, h=22, set_=True)
button("a_take", 412, ROW_A + 14, "TAKE ALL", w=54, h=22)

# Sections: SONG, eight numbered section keys (named on a strip of tape), and FOLLOW over MARK.
SECT_Y = 604
SECT_NAMES = ["intro", "verse", "pre", "chorus", "bridge", "solo", "break", "outro"]   # the tape, as the mockup shows it
NUM_SECT = 8
SECT_PITCH = 36.0
button("sec_song", 58, SECT_Y, "SONG", w=40, h=22, key=True, selected=False)
SECT_KEYS = ["sec_song"]
for i in range(NUM_SECT):
    button(f"sec_{i + 1}", 104 + i * SECT_PITCH, SECT_Y, str(i + 1), w=30, h=22, key=True, selected=False)
    SECT_KEYS.append(f"sec_{i + 1}")
button("sec_follow", 414, SECT_Y - 13, "FOLLOW", w=52, h=20, set_=True)
button("sec_mark", 414, SECT_Y + 14, "MARK", w=52, h=20)
SECT_TAPE = {"x": 104 + (NUM_SECT - 1) * SECT_PITCH / 2, "y": 580, "w": NUM_SECT * SECT_PITCH + 4, "h": 17, "angle": -0.4}
SECT_LABEL = {"x": 206, "y": 629, "text": "SECTIONS", "x0": 38, "x1": 374}

# ---- the six processing cassettes: an IDEA lamp, three knobs, a switch, an IN button each
STRIP_ROWS = (186, 292, 398)
STRIP_SW = 512
IDEA_Y = 116


def strip(cid, idea, k1, k2, k3, sw, in_set=True):
    x = cx(cas(cid))
    lamp(f"{cid}_idea", x, IDEA_Y, 3.6, "amber", set_=idea, legend="IDEA")
    for spec, y in zip((k1, k2, k3), STRIP_ROWS):
        pid, name, lab, ticks, sub, set_ = spec
        knob(pid, "med", x, y, name, labels(lab), ticks=ticks, sub=sub, set_=set_)
    if sw[0] == "toggle":
        _, pid, up, down, name, set_ = sw
        toggle(pid, x, STRIP_SW - 4, up, down, name=name, set_=set_)
    else:
        _, pid, name, texts, spacing, set_ = sw
        selector(pid, x, STRIP_SW + 8, name, texts, spacing=spacing, set_=set_)
    button(f"{cid}_in", x, IN_Y, "IN", set_=in_set)


BIP = [0, 0.5, 1]
strip("clean", True,
      ("c_res", "RESONANCE", [(0, "0"), (1, "10")], TEN, None, 0.35),
      ("c_harsh", "HARSH", [(0, "0"), (1, "10")], TEN, None, 0.25),
      ("c_rumble", "RUMBLE", [(0, "OFF"), (0.5, "30"), (1, "80")], TEN, "Hz", 0.3),
      ("toggle", "c_hear", "REMOVED", "RESULT", "HEAR", "down"))
strip("tone", True,
      ("t_low", "LOW", [(0, "-6"), (0.5, "0"), (1, "+6")], ticks_lin(-6, 6, 1), "dB", 0.5),
      ("t_pres", "PRESENCE", [(0, "-6"), (0.5, "0"), (1, "+6")], ticks_lin(-6, 6, 1), "dB", lin(-6, 6, -0.8)),
      ("t_air", "AIR", [(0, "-6"), (0.5, "0"), (1, "+6")], ticks_lin(-6, 6, 1), "dB", lin(-6, 6, 1.5)),
      ("toggle", "t_mode", "DYNAMIC", "STATIC", "MODE", "up"))
strip("dyn", False,
      ("d_glue", "GLUE", [(0, "0"), (1, "10")], TEN, None, 0.4),
      ("d_punch", "PUNCH", [(0, "LESS"), (0.5, "0"), (1, "MORE")], TEN, None, 0.6),
      ("d_even", "EVEN", [(0, "0"), (1, "10")], TEN, None, 0.2),
      ("toggle", "d_chan", "M / S", "L / R", "CHANNELS", "down"))
strip("col", False,
      ("c_drive", "DRIVE", [(0, "0"), (1, "10")], TEN, None, 0.3),
      ("c_shade", "SHADE", [(0, "DARK"), (1, "BRIGHT")], TEN, None, 0.45),
      ("c_mix", "MIX", [(0, "0"), (1, "100")], TEN, "%", 0.6),
      ("sel", "c_type", "TYPE", ["TAPE", "IRON", "VALVE"], 40.0, 0))
strip("st", False,
      ("s_width", "WIDTH", [(0, "0"), (0.5, "100"), (1, "200")], ticks_lin(0, 200, 25), "%", 0.54),
      ("s_mono", "BASS MONO", [(0, "OFF"), (0.5, "120"), (1, "300")], TEN, "Hz", 0.4),
      ("s_centre", "CENTRE", [(0, "-3"), (0.5, "0"), (1, "+3")], ticks_lin(-3, 3, 0.5), "dB", 0.5),
      ("toggle", "s_hear", "MONO", "STEREO", "HEAR", "down"), in_set=False)
strip("loud", True,
      ("l_target", "TARGET", [(0, "-16"), (0.5, "-11"), (1, "-6")], ticks_lin(-16, -6, 1), "LUFS", lin(-16, -6, -9.4)),
      ("l_ceiling", "CEILING", [(0, "-2"), (1, "0")], ticks_lin(-2, 0, 0.25), "dBTP", 0.5),
      ("l_clip", "CLIPPER", [(0, "0"), (1, "10")], TEN, None, 0.3),
      ("toggle", "l_guard", "ON", "OFF", "GUARD", "up"))

# ---- OUTPUT: loudness and punch meters, the two numbers that matter, how you listen
c = cx(cas("out"))
L_, R_ = c - 56, c + 56
MET_W, MET_H, MET_Y = 104, 84, 148
CONTROLS.append({"id": "m_loud", "type": "meter", "kind": "lufs", "x": L_, "y": MET_Y, "w": MET_W, "h": MET_H})
CONTROLS.append({"id": "m_punch", "type": "meter", "kind": "plr", "x": R_, "y": MET_Y, "w": MET_W, "h": MET_H})
METER_SET = {"m_loud": -10.2, "m_punch": 9.0}     # where the mockup's needles rest (LUFS, dB)
lamp("over", R_ + 30, 206, 4.2, "red", set_=False, legend="OVER")
lamp("guarding", L_ + 30, 206, 4.2, "amber", set_=True, legend="GUARD")
READOUTS = [
    {"id": "r_lufs", "x": L_, "y": 244, "w": 70, "h": 28, "text": "-9.4", "name": "INTEGRATED", "sub": "LUFS"},
    {"id": "r_tp", "x": R_, "y": 244, "w": 70, "h": 28, "text": "-1.0", "name": "TRUE PEAK", "sub": "dBTP"},
]
selector("o_listen", L_, 356, "LISTEN ON", ["DESK", "PHONE", "BUDS", "CAR", "CLUB"], spacing=36.0, set_=0, label_r=44.0)
toggle("o_codec", R_, 348, "ON", "OFF", name="CODEC", set_="down")
selector("o_dither", L_, 464, "DITHER", ["OFF", "24", "16"], spacing=40.0, set_=1)
knob("o_monitor", "med", R_, 464, "MONITOR", labels([(0, "-30"), (1, "0")]), ticks=ticks_lin(-30, 0, 5), sub="dB", set_=0.72)
button("o_before", 976, 566, "BEFORE", w=54, h=22)
button("o_match", 1046, 566, "MATCHED", w=62, h=22, set_=True)
button("o_blind", 1118, 566, "BLIND", w=48, h=22)
BYPASS_LABEL = {"x": 1047, "y": 590, "text": "COMPARE", "x0": 946, "x1": 1146}
lamp("power", 1150, 634, 7, "amber", set_=True)

# ---- rails
BRAND = {"text": "RUMPUS ROOM RECORDS", "x": 44, "y": 30, "size": 21}
TYPE_PLATE = {"text": "TYPE LK 7", "x": 44, "y": 51, "size": 8.5}
# The model's nameplate: black anodised aluminium, engraved through to bright metal, riveted to the rail.
NAMEPLATE = {"x": 760, "y": 32, "w": 300, "h": 46, "name": "LACQUER", "sub": "MASTERING DESK  ·  WITH ASSISTANT"}
PRESET_TAPE = {"x": 530, "y": RAIL_MID, "w": 420, "h": 34, "angle": 0.4}
button("preset_prev", 292, RAIL_MID, "◂", w=22, h=22, small=True)
button("preset_next", 768, RAIL_MID, "▸", w=22, h=22, small=True)
button("ab_a", 846, RAIL_MID - 4, "A", w=26, h=22, small=True, set_=True)
button("ab_b", 880, RAIL_MID - 4, "B", w=26, h=22, small=True)
button("menu", 940, RAIL_MID, "SET", w=40, h=22, small=True)
# The hidden space: a quarter-turn latch on the bottom rail lets the SPACE tray out from under the desk.
SPACE_LATCH = {"x": 60, "y": RAIL_MID, "r": 8.0}
lamp("space_on", 146, RAIL_MID, 3.6, "amber", set_=True)
RAIL_LEGENDS = [
    {"text": "SPACE", "x": 102, "y": RAIL_MID, "size": 9.0},
    {"text": "PRESET", "x": 236, "y": RAIL_MID, "size": 9.0},
    {"text": "COMPARE", "x": 863, "y": RAIL_MID + 16, "size": 7.4},
]
PRESET_TEXT = "room to breathe"

# ---- character
SERIAL_PLATE = {"x": 1086, "y": RAIL_MID, "w": 148, "h": 32,
                "lines": ["RUMPUS ROOM RECORDS", "LACQUER  TYPE LK 7  No. 0042"]}
STICKER = {"x": 1012, "y": 626, "w": 84, "h": 46, "angle": -2.5,
           "printed": ["INSPECTED", "No. 0042"], "hand": "14.3.68  HW"}
COFFEE = {"x": 1092, "y": 30, "r": 21}
GREASE = {"control": "l_target", "label": "-11", "note": "plenty"}

SCREWS = [(12, 32), (1188, 32), (12, RAIL_MID), (1188, RAIL_MID)]
for cs in CASSETTES:
    SCREWS += [(cs["x0"] + 9, 72), (cs["x1"] - 9, 72), (cs["x0"] + 9, 648), (cs["x1"] - 9, 648)]
CRT_SCREWS = [(CRT["x"] + sx * (CRT["w"] / 2 - 9), CRT["y"] + sy * (CRT["h"] / 2 - 9)) for sx in (-1, 1) for sy in (-1, 1)]


SEL_ANGLES = sorted({round(a, 3) for c in CONTROLS if c["type"] == "knob" and c["kind"] == "sel" for a in c["steps"]})
CAP_R = 0.74 * 0.66


def lit_rect(c):
    """The patch of panel that changes when a button or lamp lights, in whole points."""
    if c["type"] == "lamp":
        m = c["r"] + 9
        return (int(c["x"] - m), int(c["y"] - m), int(2 * m) + 1, int(2 * m) + 1)
    mx, my = 7, 7
    x0, y0 = int(c["x"] - c["w"] / 2 - mx), int(c["y"] - c["h"] / 2 - my)
    return (x0, y0, int(c["x"] + c["w"] / 2 + mx) + 1 - x0, int(c["y"] + c["h"] / 2 + my) + 1 - y0)


# Lit crops the mockup lays over the photo: each screen and section key lit green and pressed in, MARK lit amber.
LIT_GROUPS = {kid: [kid] for kid, _ in SCREEN_KEYS}
LIT_GROUPS.update({"key_" + k: [k] for k in SECT_KEYS})
LIT_GROUPS["mark"] = ["sec_follow", "sec_mark"]
LIT_GROUPS["asst"] = ["a_listen", "a_take"]
LIT_GROUPS["ideas"] = [f"{c}_idea" for c in ("clean", "tone", "dyn", "col", "st", "loud")]
LIT_GROUPS["ins"] = [f"{c}_in" for c in ("clean", "tone", "dyn", "col", "st", "loud")]
LIT_GROUPS["meters"] = ["guarding", "over"]
LIT_GROUPS["out"] = ["o_before", "o_match", "o_blind"]
LIT_GROUPS["power"] = ["power"]
LIT_GROUPS["rail"] = ["space_on", "ab_a", "ab_b"]


# ---------------------------------------------------------------------------
# The SPACE tray: long player's space, on a pull-out tray under the desk. Its own canvas.
TRAY_W, TRAY_H = 1200, 176
TRAY_CONTROLS = []


def _tray(fn, *a, **k):
    n = len(CONTROLS)
    fn(*a, **k)
    TRAY_CONTROLS.append(CONTROLS.pop(n))


TY = 104
TRAY_PLATE = {"x": 84, "y": 58, "w": 116, "h": 40, "name": "SPACE", "sub": "SET PER SECTION"}
_tray(button, "sp_in", 84, 126, "IN", set_=True)
_tray(selector, "sp_type", 216, TY, "TYPE", ["CLEAN", "STUDIO", "PLATE", "SPRING", "CHAMBER", "ROOM"], spacing=36.0, set_=2, label_r=50.0)
_tray(knob, "sp_amount", "med", 318, TY, "AMOUNT", labels([(0, "-60"), (1, "-15")]), ticks=ticks_lin(-60, -15, 5), sub="dB", set_=lin(-60, -15, -32))
_tray(knob, "sp_decay", "med", 390, TY, "DECAY", labels([(0, "0.3"), (1, "2.5")]),
      ticks=[logn(0.3, 2.5, v) for v in (0.3, 0.5, 0.7, 1.0, 1.5, 2.0, 2.5)], sub="s", set_=logn(0.3, 2.5, 1.4))
_tray(knob, "sp_tone", "med", 462, TY, "TONE", labels([(0, "DARK"), (1, "BRIGHT")]), ticks=TEN, set_=0.5)
_tray(knob, "sp_pre", "med", 534, TY, "PRE-DELAY", labels([(0, "0"), (1, "100")]), ticks=TEN, sub="ms", set_=0.2)
_tray(knob, "sp_low", "med", 606, TY, "LOW CUT", labels([(0, "100"), (1, "600")]),
      ticks=[logn(100, 600, v) for v in (100, 150, 200, 300, 400, 600)], sub="Hz", set_=logn(100, 600, 200))
_tray(knob, "sp_high", "med", 678, TY, "HIGH CUT", labels([(0, "2"), (1, "16")]),
      ticks=[logn(2, 16, v) for v in (2, 3, 4, 6, 8, 12, 16)], sub="kHz", set_=logn(2, 16, 8))
_tray(knob, "sp_gap", "med", 750, TY, "GAP-FILL", labels([(0, "0"), (1, "100")]), ticks=TEN, sub="%", set_=0.6)
_tray(selector, "sp_apply", 834, TY, "APPLY TO", ["MID", "SIDE", "BOTH"], spacing=40.0, set_=1)
_tray(knob, "sp_a", "med", 906, TY, "A", labels([(0, "0"), (1, "10")]), ticks=TEN, set_=0.4)
_tray(knob, "sp_b", "med", 968, TY, "B", labels([(0, "0"), (1, "10")]), ticks=TEN, set_=0.6)
TRAY_TABLE = {"x": 1010, "y": 60, "step": 13.0, "cols": (1010, 1062, 1122),
              "rows": [("TYPE", "A", "B"), ("CLEAN", "DIFFUSION", "EARLY"), ("STUDIO", "DAMPING", "TAIL MOD"),
                       ("PLATE", "DAMPING", "BRIGHT"), ("SPRING", "TENSION", "DRIP"), ("CHAMBER", "ABSORPTION", "SIZE"),
                       ("ROOM", "SIZE", "EARLY")]}
TRAY_SCREWS = [(12, 14), (1188, 14), (12, 162), (1188, 162)]


def control(pid):
    return next(c for c in CONTROLS if c["id"] == pid)


def set_angle(ctl):
    """The angle the mockup puts a knob at."""
    if ctl["kind"] == "sel":
        return ctl["steps"][ctl["set"]]
    return -SWEEP + 2 * SWEEP * ctl["set"]

# Every angle a selector can rest at, the tray's included (the frames of knob_sel.png).
SEL_ANGLES = sorted({round(a, 3) for c in CONTROLS + TRAY_CONTROLS if c["type"] == "knob" and c["kind"] == "sel" for a in c["steps"]})
