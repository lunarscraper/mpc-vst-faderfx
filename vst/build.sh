#!/usr/bin/env bash
# Build Fader FX (armhf .so + skin + plugin-list entry into vst/build/) with mpc-vst-plugins'
# standard port builder. $MPC_VST = a checkout of sd88me/mpc-vst-plugins (CI sets it).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst-plugins}" && pwd)"
bash "$MPC_VST/tools/build_port.sh" vst.json
