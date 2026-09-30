#!/usr/bin/env python3
"""Nachbearbeitung des generierten Skins: Slider-Filmstrips unter 16384 px bringen.

Problem: tools/shadow_skin.py baut slider_h/slider_v als Filmstrip mit 128 QUADRATISCHEN Frames
(Kantenlänge = längste Seite). Der FADER (1000x60) wird so 1000 x 128000 px hoch. MPC zeichnet
Bilder über 16384 px falsch (docs/PORTING.md): der Frame-Versatz wächst mit dem Wert, der Fader
"wandert" nach oben.

Lösung (experimentell, auf dem Gerät zu prüfen): Jeden Frame auf die echte Slider-Größe
zuschneiden (1000x60 -> 128 x 60 = 7680 px hoch) und in TUI.json die Bounds anpassen:
Knob-Bounds = Slidergröße, Name/Value-Labels und die platzierte Komponente um den
weggefallenen Rand verschieben.

Zusätzlich: Beschriftungen unter den Schaltern (toggle) größer. shadow_skin.py setzt sie fest auf
15 px (label_scale wirkt dort nicht); hier werden sie auf TOGGLE_LABEL_PX vergrößert.

Aufruf: fix_slider.py <Ordner "Plugin Skins">
"""
import glob
import json
import os
import re
import sys

from PIL import Image

Image.MAX_IMAGE_PIXELS = None
LIMIT = 16384
TOGGLE_LABEL_PX = 21.0   # Schriftgröße der Schalter-Beschriftung (Generator: 15)


def parse_bounds(s):
    return [int(float(v)) for v in s.split()]


def fmt(b):
    return " ".join(str(v) for v in b)


def main(skin_dir):
    tui_path = os.path.join(skin_dir, "TUI.json")
    tui = json.load(open(tui_path, encoding="utf-8"))
    defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
    fixed = {}

    for d in defs:
        m = re.match(r"shSlider_([hv])_(\d+)x(\d+)", d["key"])
        if not m:
            continue
        vert, w, h = m.group(1) == "v", int(m.group(2)), int(m.group(3))
        sq = max(w, h)
        if w == h:
            continue
        comps = d["value"]["componentsData"]
        knob = next((c for c in comps if c["componentData"]["type"] == "Knob"), None)
        if not knob:
            continue
        strip = os.path.join(skin_dir, knob["componentData"]["data"]["filmStrip"])
        img = Image.open(strip)
        if img.size[0] != sq or img.size[1] % sq:
            print("fix_slider: %s hat unerwartete Größe %s, übersprungen" % (strip, img.size))
            continue
        n = img.size[1] // sq
        px, py = ((sq - w) // 2, 0) if vert else (0, (sq - h) // 2)

        # 1. Filmstrip: jeden quadratischen Frame auf w x h zuschneiden
        out = Image.new("RGBA", (w, h * n), (0, 0, 0, 0))
        src = img.convert("RGBA")
        for k in range(n):
            out.paste(src.crop((px, k * sq + py, px + w, k * sq + py + h)), (0, k * h))
        out.save(strip)

        # 2. Bounds in der Definition: Knob auf Slidergröße, Rest um py nach oben
        for c in comps:
            b = parse_bounds(c["bounds"]["bounds"])
            if c is knob:
                b = [b[0] + px, b[1], w, h]
            elif c["componentData"]["type"] == "Focus":
                b[3] -= py
            else:
                b[1] -= py
            c["bounds"]["bounds"] = fmt(b)
        fixed[d["key"]] = py
        print("fix_slider: %s  %dx%d -> %dx%d px (%d Frames)" % (os.path.basename(strip), sq, sq * n, w, h * n, n))

    # 3. Platzierte Komponenten: um py nach unten schieben, Höhe um py kürzen
    for d in defs:
        for c in d["value"].get("componentsData", []):
            py = fixed.get(c["componentData"]["type"])
            if py is None:
                continue
            b = parse_bounds(c["bounds"]["bounds"])
            b[1] += py
            b[3] -= py
            c["bounds"]["bounds"] = fmt(b)

    # 4. Schalter-Beschriftungen vergrößern (Label "Name" in allen shToggle-Definitionen)
    grow = {}
    for d in defs:
        if not d["key"].startswith("shToggle"):
            continue
        for c in d["value"]["componentsData"]:
            cd = c["componentData"]
            if cd["type"] != "Label" or cd["data"].get("type") != "Name":
                continue
            font = cd["data"]["textStyle"]["font"]
            if font["height"] >= TOGGLE_LABEL_PX:
                continue
            b = parse_bounds(c["bounds"]["bounds"])
            extra = int(round(TOGGLE_LABEL_PX - font["height"])) + 4
            font["height"] = TOGGLE_LABEL_PX
            b[3] += extra
            c["bounds"]["bounds"] = fmt(b)
            grow[d["key"]] = extra
        if d["key"] in grow:
            for c in d["value"]["componentsData"]:
                if c["componentData"]["type"] == "Focus":
                    b = parse_bounds(c["bounds"]["bounds"])
                    b[3] += grow[d["key"]]
                    c["bounds"]["bounds"] = fmt(b)
    for d in defs:
        for c in d["value"].get("componentsData", []):
            extra = grow.get(c["componentData"]["type"])
            if extra:
                b = parse_bounds(c["bounds"]["bounds"])
                b[3] += extra
                c["bounds"]["bounds"] = fmt(b)
    if grow:
        print("fix_slider: Schalter-Beschriftung auf %g px vergrößert (%s)" % (TOGGLE_LABEL_PX, ", ".join(grow)))

    json.dump(tui, open(tui_path, "w", encoding="utf-8"), indent=1)

    too_tall = []
    for f in glob.glob(os.path.join(skin_dir, "*.png")):
        with Image.open(f) as im:
            if im.size[1] > LIMIT:
                too_tall.append("%s (%d px)" % (os.path.basename(f), im.size[1]))
    if too_tall:
        print("fix_slider: WARNUNG, weiterhin über %d px: %s" % (LIMIT, ", ".join(too_tall)))
    elif not fixed and not grow:
        print("fix_slider: nichts zu tun")


if __name__ == "__main__":
    main(sys.argv[1])
