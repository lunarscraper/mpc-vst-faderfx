#!/usr/bin/env bash
# Build Fader FX (armhf .so + skin + plugin-list entry into vst/build/) with mpc-vst-plugins'
# standard port builder. $MPC_VST = a checkout of open-mpc/mpc-vst-plugins (CI sets it).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst-plugins}" && pwd)"
bash "$MPC_VST/tools/build_port.sh" vst.json

# Force-Look malen (force_look.py), danach Slider-Filmstrips auf echte Slidergröße zuschneiden
# (FADER: 128000 px -> 7680 px hoch, weil MPC Bilder über 16384 px falsch zeichnet; siehe fix_slider.py).
# build_port.sh führt vst.json "skin_post" nicht aus (anders als gen_vst.py bei Rattler/Wumms),
# darum ruft dieses Skript force_look.py selbst auf.
for SKIN in build/skin/*/"Plugin Skins"; do
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 force_look.py '$SKIN' && PYTHONPATH=/tmp/p python3 fix_slider.py '$SKIN'"
done
