#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan): engine + mpc-vst-plugins' generic wrapper driven with real
# audio by tests/fx_test.c. (tools/test_port.sh is for instruments: it passes no audio input.)
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst-plugins}" && pwd)"
docker run --rm -v "$PWD/..":/b -v "$MPC_VST":/mv:ro -w /b/vst gcc:12 bash -euc '
  set -o pipefail
  mkdir -p build/x86
  SAN="-fsanitize=address,undefined -g -O1"
  gcc $SAN -std=gnu11 -Wall -Wextra -Ibuild -I../src -c ../src/djfx.c -o build/x86/djfx.o
  gcc $SAN -std=gnu11 -Wno-unused-parameter -Ibuild -c /mv/wrapper/vst2_wrap.c -o build/x86/wrap.o
  gcc $SAN -std=gnu11 -Wall -c ../tests/fx_test.c -o build/x86/fx_test.o
  gcc $SAN -o build/x86/fx_test build/x86/*.o -lm -ldl
  ./build/x86/fx_test
'
