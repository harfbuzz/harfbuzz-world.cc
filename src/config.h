/* Configuration for HarfBuzz World wasm build (Emscripten).
 *
 * HB_TINY strips most optional features; config-override.h
 * (next door) adds back the ones the demos actually need.
 */

#define HAVE_ROUND 1
#define HB_TINY 1

#define HB_HAS_RASTER 1
#define HB_HAS_VECTOR 1
#define HB_HAS_SUBSET 1
/* HB_HAS_GPU intentionally off here -- the gpu tab on this
 * site embeds harfbuzz.github.io/hb-gpu-demo via iframe, so
 * we don't need to ship hb-gpu's runtime in our own wasm. */

#define HAVE_CONFIG_OVERRIDE_H 1
