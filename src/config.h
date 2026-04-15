/* Configuration for HarfBuzz World wasm build (Emscripten).
 *
 * HB_TINY strips most optional features; config-override.h
 * (next door) adds back the ones the demos actually need.
 */

#define HAVE_ROUND 1
#define HB_TINY 1

#define HB_HAS_RASTER 1
#define HB_HAS_VECTOR 1
#define HB_HAS_GPU    1
/* HB_HAS_SUBSET pending: HB's harfbuzz-world.cc duplicates a
 * handful of .cc files (hb-number.cc, hb-ot-cff{1,2}-table.cc)
 * across the base + subset blocks because each library was
 * historically its own TU.  Enabling subset here triggers
 * "redefinition" errors.  Fix is upstream (add include guards
 * to those files). */

#define HAVE_CONFIG_OVERRIDE_H 1
