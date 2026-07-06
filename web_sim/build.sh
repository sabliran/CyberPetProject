#!/usr/bin/env bash
# Build the CyberPet web simulator.
#
# Option A - native SDL2 desktop window (no emscripten needed):
#   ./build.sh native
#   Runs directly: ./build_native/cyberpet_sim
#
# Option B - WebAssembly, emscripten installed locally (e.g. `sudo pacman -S emscripten`):
#   ./build.sh
#
# Option C - WebAssembly via Docker (no emscripten install needed):
#   ./build.sh docker
#
# WASM output lands in dist/: cyberpet_sim.html + .js + .wasm
# Serve with: cd dist && python3 -m http.server 8000
# Or copy into dashboard: cp dist/* ../CyberPetDashboard/public/sim/
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" = "native" ]; then
  cmake -B build_native -DCMAKE_BUILD_TYPE=Release
  cmake --build build_native -j
  echo ""
  echo "Built. Run:  ./build_native/cyberpet_sim"
elif [ "${1:-}" = "docker" ]; then
  docker run --rm -v "$(pwd)/..":/src -w /src/web_sim emscripten/emsdk:3.1.61 \
    bash -c "emcmake cmake -B build_wasm -DCMAKE_BUILD_TYPE=Release && cmake --build build_wasm -j"
  mkdir -p dist
  cp build_wasm/cyberpet_sim.html build_wasm/cyberpet_sim.js build_wasm/cyberpet_sim.wasm dist/
  echo ""
  echo "Built. Try:  cd dist && python3 -m http.server 8000"
  echo "Then open:   http://localhost:8000/cyberpet_sim.html"
else
  command -v emcmake >/dev/null || { echo "emscripten not found - run './build.sh native' or './build.sh docker' instead"; exit 1; }
  emcmake cmake -B build_wasm -DCMAKE_BUILD_TYPE=Release
  cmake --build build_wasm -j
  mkdir -p dist
  cp build_wasm/cyberpet_sim.html build_wasm/cyberpet_sim.js build_wasm/cyberpet_sim.wasm dist/
  echo ""
  echo "Built. Try:  cd dist && python3 -m http.server 8000"
  echo "Then open:   http://localhost:8000/cyberpet_sim.html"
fi
