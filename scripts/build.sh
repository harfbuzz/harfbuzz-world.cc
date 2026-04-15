#!/bin/sh
#
# Build the HarfBuzz World wasm bundle.
#
# Prerequisites:
#   - HarfBuzz checkout at $HB_SRC (default ~/harfbuzz).
#   - Emscripten toolchain on PATH:  source emsdk/emsdk_env.sh
#
# Outputs hb-world.js + hb-world.wasm at the repo root, where
# the static HTML can load them via <script src="hb-world.js">.

set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"

# Pick the HarfBuzz source tree to build against:
#   1. $HB_SRC if set explicitly.
#   2. ./harfbuzz submodule (fresh-clone / CI default).
#   3. ~/harfbuzz if it exists (developer's working tree).
if [ -z "$HB_SRC" ]; then
  if [ -f "$HERE/harfbuzz/src/harfbuzz-world.cc" ]; then
    HB_SRC="$HERE/harfbuzz"
  elif [ -f "$HOME/harfbuzz/src/harfbuzz-world.cc" ]; then
    HB_SRC="$HOME/harfbuzz"
  fi
fi

if [ -z "$HB_SRC" ] || [ ! -f "$HB_SRC/src/harfbuzz-world.cc" ]; then
  echo "error: cannot find HarfBuzz source" >&2
  echo "       set HB_SRC=/path/to/harfbuzz, or" >&2
  echo "       run 'git submodule update --init harfbuzz'" >&2
  exit 1
fi
echo "Using HarfBuzz source at: $HB_SRC"

em++ \
  -std=c++17 \
  -Oz -flto \
  -I"$HB_SRC/src" \
  -I"$HERE/src" \
  -DHAVE_CONFIG_H \
  "$HB_SRC/src/harfbuzz-world.cc" \
  "$HERE/src/bindings.cc" \
  -sALLOW_MEMORY_GROWTH=1 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createHbWorld \
  -sEXPORTED_FUNCTIONS='["_web_render_svg","_web_shape_json","_web_free_string","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","HEAPU8","lengthBytesUTF8"]' \
  -o "$HERE/hb-world.js"

echo "Built: hb-world.{js,wasm}"
echo "Serve: python3 -m http.server -d $HERE"
