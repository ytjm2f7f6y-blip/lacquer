"""Packs the renders in art/build into the plugin's assets.

    python3 art/pack_assets.py [--kontur ../kontur]

- panel.jpg          the bare faceplate        (render.py panel --state bare)
- tray.jpg           the SPACE tray, bare      (render.py tray --state bare)
- lit_<group>.png    the lit patches: green for the screen and section keys (--state green), amber for the rest
- knob_sel.png       the selector at each angle it can rest at (render.py knob_sel)
- knob_large/med, toggle_up/down, glass_meter: the same parts as Kontur's, copied from its assets
"""
import argparse
import os
import shutil

import numpy as np
from PIL import Image

import layout as L

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
ASSETS = os.path.join(HERE, "..", "plugin", "assets")


def clean_catcher(im, frames=1):
    """Sprites rendered on a shadow catcher pick up a faint, even tint across the whole box.
    Take that level out, and fade the last few pixels to nothing, so the box never shows."""
    a = np.asarray(im.convert("RGBA")).astype(np.float32).copy()
    h = a.shape[0] // frames
    for f in range(frames):
        fr = a[f * h:(f + 1) * h]
        al = fr[..., 3]
        border = np.concatenate([al[0, :], al[-1, :], al[:, 0], al[:, -1]])
        bgl = float(np.median(border))
        if bgl > 0:
            fr[..., 3] = np.clip((al - bgl) / max(1e-3, 255 - bgl) * 255, 0, 255)
        n = 6
        ramp = np.minimum(np.minimum(np.arange(fr.shape[0])[:, None], np.arange(fr.shape[0])[::-1, None]),
                          np.minimum(np.arange(fr.shape[1])[None, :], np.arange(fr.shape[1])[None, ::-1]))
        fr[..., 3] *= np.clip(ramp / n, 0, 1)
        a[f * h:(f + 1) * h] = fr
    return Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8), "RGBA")


def first(*names):
    for n in names:
        p = os.path.join(BUILD, n)
        if os.path.exists(p):
            return p
    raise SystemExit("missing render: " + " or ".join(names))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--kontur", default=os.path.join(HERE, "..", "..", "kontur"))
    args = ap.parse_args()
    os.makedirs(ASSETS, exist_ok=True)

    p = first("bare_panel.jpg", "bare_panel_preview.jpg")
    Image.open(p).convert("RGB").save(os.path.join(ASSETS, "panel.jpg"), quality=90, subsampling=0, optimize=True)
    print("panel from", os.path.basename(p))
    p = first("bare_tray.jpg", "bare_tray_preview.jpg")
    Image.open(p).convert("RGB").save(os.path.join(ASSETS, "tray.jpg"), quality=90, subsampling=0, optimize=True)
    print("tray from", os.path.basename(p))

    for g in list(L.LIT_GROUPS) + ["tray"]:
        src = os.path.join(BUILD, "lit", ("green_" if g.startswith("key_") else "amber_") + g + ".png")
        im = Image.open(src).convert("RGB")
        im.save(os.path.join(ASSETS, f"lit_{g}.png"), optimize=True)
    print("lit patches:", ", ".join(list(L.LIT_GROUPS) + ["tray"]))

    strip = Image.open(os.path.join(BUILD, "strips", "knob_sel.png"))
    n = strip.size[1] // strip.size[0]
    assert n == len(L.SEL_ANGLES), (n, len(L.SEL_ANGLES))
    clean_catcher(strip, n).save(os.path.join(ASSETS, "knob_sel.png"), optimize=True)
    print("knob_sel:", n, "frames")

    ka = os.path.join(args.kontur, "plugin", "assets")
    for f in ("knob_large.jpg", "knob_large_mask.png", "knob_med.jpg", "knob_med_mask.png", "toggle_up.png", "toggle_down.png"):
        shutil.copy(os.path.join(ka, f), os.path.join(ASSETS, f))
    shutil.copy(os.path.join(ka, "glass_vu.png"), os.path.join(ASSETS, "glass_meter.png"))
    print("knobs, toggles and glass from", os.path.relpath(ka))
