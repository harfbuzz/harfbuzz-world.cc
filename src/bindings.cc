/*
 * HarfBuzz World — wasm bindings.
 *
 * Exports a narrow C surface for the HTML/JS demos to call.
 * Each entry point owns its own font-blob ownership: callers
 * pass raw bytes + length + text and get back a malloc'd
 * string the JS side reads via Module.UTF8ToString and frees
 * via web_free_string when done.
 */

#include <hb.h>
#include <hb-ot.h>
#include <hb-raster.h>
#include <hb-subset.h>
#include <hb-vector.h>

#include <emscripten.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

/* Sub-pixel precision for shaped glyph positions.  Same
 * 26.6-fixed-point convention as the in-tree hb-vector /
 * hb-raster utils (and FreeType): shape at font_size * SCALE,
 * then tell the render context to divide by SCALE to land on
 * pixels. */
#define SUBPIXEL_BITS 6
#define SCALE (1 << SUBPIXEL_BITS)

/* JSON strings can grow to six times their UTF-8 byte length.
 * Append to a growable buffer and preserve embedded control bytes. */
static void
append_json_string (std::string &out, const char *text, size_t len)
{
  const char hex[] = "0123456789abcdef";
  out += '"';
  for (size_t i = 0; i < len; i++)
  {
    unsigned char c = (unsigned char) text[i];
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += c;
    }
    else if (c < 0x20)
    {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 0xf];
    }
    else
      out += c;
  }
  out += '"';
}

static void
append_json_tag (std::string &out, hb_tag_t tag)
{
  char text[4];
  hb_tag_to_string (tag, text);
  append_json_string (out, text, sizeof text);
}

extern "C" {

static unsigned g_face_index = 0;

EMSCRIPTEN_KEEPALIVE
void web_set_face_index (unsigned index)
{
  g_face_index = index;
}

EMSCRIPTEN_KEEPALIVE
void web_free_string (char *s)
{
  free (s);
}

/* Check font data before the shell replaces the active font. */
EMSCRIPTEN_KEEPALIVE
unsigned web_font_face_count (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  unsigned count = hb_face_count (blob);
  hb_blob_destroy (blob);
  return count;
}

/* Return the font's typographic family (name id 16), falling
 * back to legacy family (id 1), as a malloc'd UTF-8 string.
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_font_family (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("");
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);

  const hb_ot_name_id_t ids[] = { HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY,
                                  HB_OT_NAME_ID_FONT_FAMILY };
  for (hb_ot_name_id_t id : ids)
  {
    char buf[256];
    unsigned sz = sizeof buf;
    if (hb_ot_name_get_utf8 (face, id, HB_LANGUAGE_INVALID, &sz, buf) > 0)
    {
      hb_face_destroy (face);
      return strdup (buf);
    }
  }
  hb_face_destroy (face);
  return strdup ("");
}

/* JSON-describe a font's structure: total glyph count,
 * total Unicode coverage, and per-table sizes (tag + bytes).
 * Used by the subset tab to show before/after deltas.
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_font_stats (const uint8_t *font_bytes, unsigned font_len,
                      unsigned face_index)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("{\"num_glyphs\":0,\"num_unicodes\":0,\"tables\":[]}");
  hb_face_t *face = hb_face_create (blob, face_index);
  hb_blob_destroy (blob);

  unsigned num_glyphs = hb_face_get_glyph_count (face);
  hb_set_t *unicodes = hb_set_create ();
  hb_face_collect_unicodes (face, unicodes);
  unsigned num_unicodes = hb_set_get_population (unicodes);
  hb_set_destroy (unicodes);

  unsigned table_count = hb_face_get_table_tags (face, 0, nullptr, nullptr);
  hb_tag_t *tags = (hb_tag_t *) calloc (table_count ? table_count : 1, sizeof (hb_tag_t));
  if (table_count) hb_face_get_table_tags (face, 0, &table_count, tags);

  std::string out = "{\"num_glyphs\":" + std::to_string (num_glyphs) +
                    ",\"num_unicodes\":" + std::to_string (num_unicodes) +
                    ",\"tables\":[";
  for (unsigned i = 0; i < table_count; i++)
  {
    hb_blob_t *t = hb_face_reference_table (face, tags[i]);
    unsigned len = hb_blob_get_length (t);
    hb_blob_destroy (t);
    if (i) out += ',';
    out += "{\"tag\":";
    append_json_tag (out, tags[i]);
    out += ",\"size\":" + std::to_string (len) + "}";
  }
  out += "]}";

  free (tags);
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

/* Variation state.  Set once by web_set_variations() and
 * applied to every font the render helpers create.  Kept as
 * the raw comma-separated string so the common case (no
 * variations) is a single empty-string check. */
static char g_variations[256];

EMSCRIPTEN_KEEPALIVE
void web_set_variations (const char *s)
{
  if (!s) { g_variations[0] = 0; return; }
  unsigned n = strlen (s);
  if (n >= sizeof g_variations) n = sizeof g_variations - 1;
  memcpy (g_variations, s, n);
  g_variations[n] = 0;
}

/* Feature string.  Comma-separated list of feature settings
 * ("ss01=1,liga=0") applied to every hb_shape call. */
static char g_features[512];
static hb_feature_t g_feature_list[64];
static unsigned g_feature_count = 0;

EMSCRIPTEN_KEEPALIVE
void web_set_features (const char *s)
{
  if (!s) { g_features[0] = 0; g_feature_count = 0; return; }
  unsigned n = strlen (s);
  if (n >= sizeof g_features) n = sizeof g_features - 1;
  memcpy (g_features, s, n);
  g_features[n] = 0;
  g_feature_count = 0;
  const char *p = g_features;
  while (p && *p && g_feature_count < 64)
  {
    const char *end = strchr (p, ',');
    int len = end ? (int) (end - p) : (int) strlen (p);
    if (hb_feature_from_string (p, len, &g_feature_list[g_feature_count]))
      g_feature_count++;
    p = end ? end + 1 : nullptr;
  }
}

/* Selected CPAL palette index.  Applied to vector_paint /
 * raster_paint contexts in the render helpers below. */
static unsigned g_palette = 0;

EMSCRIPTEN_KEEPALIVE
void web_set_palette (unsigned idx)
{
  g_palette = idx;
}

/* Foreground / background colors for rendering.
 * RGBA packed as HB_COLOR (blue, green, red, alpha). */
static hb_color_t g_foreground = HB_COLOR (0, 0, 0, 255);
static hb_color_t g_background = HB_COLOR (0, 0, 0, 0);

EMSCRIPTEN_KEEPALIVE
void web_set_foreground (unsigned r, unsigned g, unsigned b, unsigned a)
{
  g_foreground = HB_COLOR (b, g, r, a);
}

EMSCRIPTEN_KEEPALIVE
void web_set_background (unsigned r, unsigned g, unsigned b, unsigned a)
{
  g_background = HB_COLOR (b, g, r, a);
}

/* Shape cluster level.  Applied to every buffer created by
 * the shape() helper.  Values match hb_buffer_cluster_level_t
 * (0 = MONOTONE_GRAPHEMES, 1 = MONOTONE_CHARACTERS,
 * 2 = CHARACTERS). */
static unsigned g_cluster_level = 0;

EMSCRIPTEN_KEEPALIVE
void web_set_cluster_level (unsigned lvl)
{
  g_cluster_level = lvl;
}

/* When set, web_subset() pins every fvar axis in the input
 * font to its current g_variations value (and omits any
 * axis not mentioned).  Result: a static instance rather
 * than a trimmed variable font. */
static bool g_subset_instantiate = true;

EMSCRIPTEN_KEEPALIVE
void web_set_subset_instantiate (int on)
{
  g_subset_instantiate = !!on;
}

static void
apply_variations (hb_font_t *font)
{
  if (!g_variations[0]) return;
  hb_variation_t vars[32];
  unsigned n = 0;
  const char *p = g_variations;
  while (p && *p && n < 32)
  {
    const char *end = strchr (p, ',');
    int len = end ? (int) (end - p) : (int) strlen (p);
    if (hb_variation_from_string (p, len, &vars[n]))
      n++;
    p = end ? end + 1 : nullptr;
  }
  hb_font_set_variations (font, vars, n);
}

static std::string
font_name (hb_face_t *face, hb_ot_name_id_t id,
           hb_language_t language = HB_LANGUAGE_INVALID)
{
  char buf[4096] = {0};
  unsigned len = sizeof buf - 1;
  hb_ot_name_get_utf8 (face, id, language, &len, buf);
  if (len >= sizeof buf) len = sizeof buf - 1;
  return std::string (buf, len);
}

static bool
face_has_table (hb_face_t *face, hb_tag_t tag)
{
  hb_blob_t *table = hb_face_reference_table (face, tag);
  bool has = hb_blob_get_length (table) != 0;
  hb_blob_destroy (table);
  return has;
}

/* Collection face labels, without changing the selected face. */
EMSCRIPTEN_KEEPALIVE
char *web_font_faces (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes, font_len,
                                            HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  unsigned count = hb_face_count (blob);
  std::string out = "[";
  for (unsigned i = 0; i < count; i++)
  {
    hb_face_t *face = hb_face_create (blob, i);
    std::string name = font_name (face, HB_OT_NAME_ID_FULL_NAME);
    if (name.empty ()) name = font_name (face, HB_OT_NAME_ID_FONT_FAMILY);
    if (i) out += ',';
    append_json_string (out, name.data (), name.size ());
    hb_face_destroy (face);
  }
  out += ']';
  hb_blob_destroy (blob);
  return strdup (out.c_str ());
}

/* Copy a selected collection face into a standalone sfnt for consumers
 * such as the GPU iframe, which accept a font but no face index. */
EMSCRIPTEN_KEEPALIVE
uint8_t *web_font_face_data (const uint8_t *font_bytes, unsigned font_len,
                             unsigned *out_len)
{
  *out_len = 0;
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes, font_len,
                                            HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  if (!blob) return nullptr;
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);
  hb_face_t *builder = hb_face_builder_create ();
  unsigned count = hb_face_get_table_tags (face, 0, nullptr, nullptr);
  std::vector<hb_tag_t> tags (count);
  if (count) hb_face_get_table_tags (face, 0, &count, tags.data ());
  bool ok = count != 0;
  for (hb_tag_t tag : tags)
  {
    /* A signature for the collection is invalid after repackaging. */
    if (tag == HB_TAG('D','S','I','G')) continue;
    hb_blob_t *table = hb_face_reference_table (face, tag);
    ok = hb_face_builder_add_table (builder, tag, table) && ok;
    hb_blob_destroy (table);
  }
  hb_blob_t *result = ok ? hb_face_reference_blob (builder) : hb_blob_get_empty ();
  unsigned len = 0;
  const char *data = hb_blob_get_data (result, &len);
  uint8_t *bytes = len ? (uint8_t *) malloc (len) : nullptr;
  if (bytes)
  {
    memcpy (bytes, data, len);
    *out_len = len;
  }
  hb_blob_destroy (result);
  hb_face_destroy (builder);
  hb_face_destroy (face);
  return bytes;
}

static void
append_json_float (std::string &out, float value)
{
  char buf[48];
  snprintf (buf, sizeof buf, "%.8g", (double) value);
  out += buf;
}

/* Render one unshaped glyph to SVG using hb-vector.  The info grids use
 * this instead of browser font rendering so cmap lookup and artwork both
 * come from the HarfBuzz font object being inspected. */
static std::string
glyph_svg (hb_face_t *face, hb_font_t *font, hb_codepoint_t gid)
{
  const int size = 52 * SCALE;
  hb_font_set_scale (font, size, size);

  hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                       hb_ot_color_has_layers (face) ||
                       face_has_table (face, HB_TAG('S','V','G',' ')) ||
                       hb_ot_color_has_png (face);
  hb_vector_paint_t *p = nullptr;
  hb_vector_draw_t *d = nullptr;
  if (is_color)
  {
    p = hb_vector_paint_create_or_fail (HB_VECTOR_FORMAT_SVG);
    if (p)
    {
      hb_vector_paint_set_palette (p, g_palette);
      hb_vector_paint_set_scale_factor (p, (float) SCALE, (float) SCALE);
      hb_vector_paint_set_foreground (p, g_foreground);
      hb_vector_paint_set_background (p, g_background);
      static unsigned prefix_counter = 0;
      char prefix[24];
      snprintf (prefix, sizeof prefix, "i%u-", ++prefix_counter);
      hb_vector_paint_set_svg_prefix (p, prefix);
    }
  }
  else
  {
    d = hb_vector_draw_create_or_fail (HB_VECTOR_FORMAT_SVG);
    if (d)
    {
      hb_vector_draw_set_scale_factor (d, (float) SCALE, (float) SCALE);
      hb_vector_draw_set_foreground (d, g_foreground);
      hb_vector_draw_set_background (d, g_background);
    }
  }
  if (!p && !d) return "";

  hb_font_extents_t fe = {0, 0, 0};
  hb_font_get_h_extents (font, &fe);
  hb_position_t advance = hb_font_get_glyph_h_advance (font, gid);
  hb_vector_extents_t logical = {0.f, (float) fe.ascender,
                                 (float) advance,
                                 (float) (fe.descender - fe.ascender)};
  if (p)
  {
    hb_vector_paint_set_extents (p, &logical);
    hb_vector_paint_glyph (p, font, gid, HB_VECTOR_EXTENTS_MODE_EXPAND);
  }
  else
  {
    hb_vector_draw_set_extents (d, &logical);
    hb_vector_draw_glyph (d, font, gid, HB_VECTOR_EXTENTS_MODE_EXPAND);
  }

  hb_blob_t *blob = p ? hb_vector_paint_render (p)
                      : hb_vector_draw_render (d);
  unsigned len = 0;
  const char *data = hb_blob_get_data (blob, &len);
  std::string svg (data ? data : "", len);
  hb_blob_destroy (blob);
  hb_vector_paint_destroy (p);
  hb_vector_draw_destroy (d);
  return svg;
}

struct tagged_name_t { hb_tag_t tag; const char *name; };

/* JSON equivalent of hb-info's default report and its long-list queries.
 * The two potentially enormous lists (characters and glyphs) are served by
 * paginated functions below. */
EMSCRIPTEN_KEEPALIVE
char *web_font_info (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("{}");
  unsigned face_count = hb_face_count (blob);
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);
  hb_font_t *font = hb_font_create (face);
  apply_variations (font);

  hb_set_t *unicode_set = hb_set_create ();
  hb_face_collect_unicodes (face, unicode_set);
  unsigned unicode_count = hb_set_get_population (unicode_set);
  hb_set_destroy (unicode_set);
  hb_font_extents_t extents = {0, 0, 0};
  hb_font_get_h_extents (font, &extents);

  std::string out = "{\"summary\":{";
  out += "\"face_count\":" + std::to_string (face_count);
  const hb_ot_name_id_t summary_ids[] = {
    HB_OT_NAME_ID_FONT_FAMILY, HB_OT_NAME_ID_FONT_SUBFAMILY,
    HB_OT_NAME_ID_UNIQUE_ID, HB_OT_NAME_ID_FULL_NAME,
    HB_OT_NAME_ID_POSTSCRIPT_NAME, HB_OT_NAME_ID_VERSION_STRING
  };
  const char *summary_keys[] = {
    "family", "subfamily", "unique_name", "full_name",
    "postscript_name", "version"
  };
  for (unsigned i = 0; i < sizeof summary_ids / sizeof summary_ids[0]; i++)
  {
    out += ",\""; out += summary_keys[i]; out += "\":";
    std::string value = font_name (face, summary_ids[i]);
    append_json_string (out, value.data (), value.size ());
  }
  out += ",\"unicode_count\":" + std::to_string (unicode_count);
  out += ",\"glyph_count\":" + std::to_string (hb_face_get_glyph_count (face));
  out += ",\"upem\":" + std::to_string (hb_face_get_upem (face));
  out += ",\"ascender\":" + std::to_string (extents.ascender);
  out += ",\"descender\":" + std::to_string (extents.descender);
  out += ",\"line_gap\":" + std::to_string (extents.line_gap);
  out += ",\"technologies\":[";
  struct technology_t { const char *name; hb_tag_t a; hb_tag_t b; hb_tag_t c; };
  const technology_t technologies[] = {
    {"TrueType outlines", HB_TAG('g','l','y','f'), 0, 0},
    {"PostScript outlines", HB_TAG('C','F','F',' '), HB_TAG('C','F','F','2'), 0},
    {"TrueType hinting", HB_TAG('f','p','g','m'), HB_TAG('p','r','e','p'), HB_TAG('c','v','t',' ')},
    {"OpenType layout", HB_OT_TAG_GSUB, HB_OT_TAG_GPOS, 0},
    {"AAT layout", HB_TAG('m','o','r','x'), HB_TAG('k','e','r','x'), 0},
    {"Graphite layout", HB_TAG('S','i','l','f'), 0, 0},
    {"WebAssembly layout", HB_TAG('W','a','s','m'), 0, 0},
    {"Legacy kerning", HB_TAG('k','e','r','n'), 0, 0},
    {"Monochrome bitmaps", HB_TAG('E','B','D','T'), 0, 0},
    {"Color bitmaps", HB_TAG('C','B','D','T'), HB_TAG('s','b','i','x'), 0},
    {"Color SVGs", HB_TAG('S','V','G',' '), 0, 0},
    {"Color paintings", HB_TAG('C','O','L','R'), 0, 0},
    {"Variations", HB_TAG('f','v','a','r'), 0, 0},
  };
  bool first = true;
  for (const auto &technology : technologies)
  {
    if (!face_has_table (face, technology.a) &&
        (!technology.b || !face_has_table (face, technology.b)) &&
        (!technology.c || !face_has_table (face, technology.c)))
      continue;
    if (!first) out += ',';
    append_json_string (out, technology.name, strlen (technology.name));
    first = false;
  }
  out += "]}";

  /* Names. */
  out += ",\"names\":[";
  unsigned name_count = 0;
  const hb_ot_name_entry_t *names = hb_ot_name_list_names (face, &name_count);
  for (unsigned i = 0; i < name_count; i++)
  {
    if (i) out += ',';
    std::string value = font_name (face, names[i].name_id, names[i].language);
    const char *language = hb_language_to_string (names[i].language);
    out += "{\"id\":" + std::to_string (names[i].name_id) + ",\"language\":";
    append_json_string (out, language ? language : "", language ? strlen (language) : 0);
    out += ",\"text\":";
    append_json_string (out, value.data (), value.size ());
    out += '}';
  }
  out += ']';

  const tagged_name_t style_tags[] = {
    {HB_STYLE_TAG_ITALIC, "italic"},
    {HB_STYLE_TAG_OPTICAL_SIZE, "optical-size"},
    {HB_STYLE_TAG_SLANT_ANGLE, "slant-angle"},
    {HB_STYLE_TAG_SLANT_RATIO, "slant-ratio"},
    {HB_STYLE_TAG_WIDTH, "width"},
    {HB_STYLE_TAG_WEIGHT, "weight"},
  };
  out += ",\"styles\":[";
  for (unsigned i = 0; i < sizeof style_tags / sizeof style_tags[0]; i++)
  {
    if (i) out += ',';
    out += "{\"tag\":"; append_json_tag (out, style_tags[i].tag);
    out += ",\"name\":"; append_json_string (out, style_tags[i].name, strlen (style_tags[i].name));
    out += ",\"value\":"; append_json_float (out, hb_style_get_value (font, (hb_style_tag_t) style_tags[i].tag));
    out += '}';
  }
  out += ']';

  const tagged_name_t metric_tags[] = {
    {HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, "horizontal-ascender"},
    {HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, "horizontal-descender"},
    {HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, "horizontal-line-gap"},
    {HB_OT_METRICS_TAG_HORIZONTAL_CLIPPING_ASCENT, "horizontal-clipping-ascent"},
    {HB_OT_METRICS_TAG_HORIZONTAL_CLIPPING_DESCENT, "horizontal-clipping-descent"},
    {HB_OT_METRICS_TAG_VERTICAL_ASCENDER, "vertical-ascender"},
    {HB_OT_METRICS_TAG_VERTICAL_DESCENDER, "vertical-descender"},
    {HB_OT_METRICS_TAG_VERTICAL_LINE_GAP, "vertical-line-gap"},
    {HB_OT_METRICS_TAG_HORIZONTAL_CARET_RISE, "horizontal-caret-rise"},
    {HB_OT_METRICS_TAG_HORIZONTAL_CARET_RUN, "horizontal-caret-run"},
    {HB_OT_METRICS_TAG_HORIZONTAL_CARET_OFFSET, "horizontal-caret-offset"},
    {HB_OT_METRICS_TAG_VERTICAL_CARET_RISE, "vertical-caret-rise"},
    {HB_OT_METRICS_TAG_VERTICAL_CARET_RUN, "vertical-caret-run"},
    {HB_OT_METRICS_TAG_VERTICAL_CARET_OFFSET, "vertical-caret-offset"},
    {HB_OT_METRICS_TAG_X_HEIGHT, "x-height"}, {HB_OT_METRICS_TAG_CAP_HEIGHT, "cap-height"},
    {HB_OT_METRICS_TAG_SUBSCRIPT_EM_X_SIZE, "subscript-em-x-size"},
    {HB_OT_METRICS_TAG_SUBSCRIPT_EM_Y_SIZE, "subscript-em-y-size"},
    {HB_OT_METRICS_TAG_SUBSCRIPT_EM_X_OFFSET, "subscript-em-x-offset"},
    {HB_OT_METRICS_TAG_SUBSCRIPT_EM_Y_OFFSET, "subscript-em-y-offset"},
    {HB_OT_METRICS_TAG_SUPERSCRIPT_EM_X_SIZE, "superscript-em-x-size"},
    {HB_OT_METRICS_TAG_SUPERSCRIPT_EM_Y_SIZE, "superscript-em-y-size"},
    {HB_OT_METRICS_TAG_SUPERSCRIPT_EM_X_OFFSET, "superscript-em-x-offset"},
    {HB_OT_METRICS_TAG_SUPERSCRIPT_EM_Y_OFFSET, "superscript-em-y-offset"},
    {HB_OT_METRICS_TAG_STRIKEOUT_SIZE, "strikeout-size"},
    {HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, "strikeout-offset"},
    {HB_OT_METRICS_TAG_UNDERLINE_SIZE, "underline-size"},
    {HB_OT_METRICS_TAG_UNDERLINE_OFFSET, "underline-offset"},
  };
  out += ",\"metrics\":[";
  for (unsigned i = 0; i < sizeof metric_tags / sizeof metric_tags[0]; i++)
  {
    hb_position_t value = 0;
    bool exact = hb_ot_metrics_get_position (font, (hb_ot_metrics_tag_t) metric_tags[i].tag, &value);
    if (!exact) hb_ot_metrics_get_position_with_fallback (font, (hb_ot_metrics_tag_t) metric_tags[i].tag, &value);
    if (i) out += ',';
    out += "{\"tag\":"; append_json_tag (out, metric_tags[i].tag);
    out += ",\"name\":"; append_json_string (out, metric_tags[i].name, strlen (metric_tags[i].name));
    out += ",\"value\":" + std::to_string (value) + ",\"fallback\":" + (exact ? "false" : "true") + "}";
  }
  out += ']';

  const tagged_name_t baseline_tags[] = {
    {HB_OT_LAYOUT_BASELINE_TAG_ROMAN, "roman"},
    {HB_OT_LAYOUT_BASELINE_TAG_HANGING, "hanging"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_FACE_BOTTOM_OR_LEFT, "ideo-face-bottom-or-left"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_FACE_TOP_OR_RIGHT, "ideo-face-top-or-right"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_FACE_CENTRAL, "ideo-face-central"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_BOTTOM_OR_LEFT, "ideo-embox-bottom-or-left"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_TOP_OR_RIGHT, "ideo-embox-top-or-right"},
    {HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_CENTRAL, "ideo-embox-central"},
    {HB_OT_LAYOUT_BASELINE_TAG_MATH, "math"},
  };
  out += ",\"baselines\":[";
  for (unsigned i = 0; i < sizeof baseline_tags / sizeof baseline_tags[0]; i++)
  {
    hb_position_t value = 0;
    bool exact = hb_ot_layout_get_baseline (font,
      (hb_ot_layout_baseline_tag_t) baseline_tags[i].tag,
      HB_DIRECTION_LTR, HB_TAG_NONE, HB_TAG_NONE, &value);
    if (!exact) hb_ot_layout_get_baseline_with_fallback (font,
      (hb_ot_layout_baseline_tag_t) baseline_tags[i].tag,
      HB_DIRECTION_LTR, HB_TAG_NONE, HB_TAG_NONE, &value);
    if (i) out += ',';
    out += "{\"tag\":"; append_json_tag (out, baseline_tags[i].tag);
    out += ",\"name\":"; append_json_string (out, baseline_tags[i].name, strlen (baseline_tags[i].name));
    out += ",\"value\":" + std::to_string (value) + ",\"fallback\":" + (exact ? "false" : "true") + "}";
  }
  out += ']';

  /* Tables. */
  unsigned table_count = hb_face_get_table_tags (face, 0, nullptr, nullptr);
  std::vector<hb_tag_t> table_tags (table_count);
  if (table_count) hb_face_get_table_tags (face, 0, &table_count, table_tags.data ());
  out += ",\"tables\":[";
  for (unsigned i = 0; i < table_count; i++)
  {
    hb_blob_t *table = hb_face_reference_table (face, table_tags[i]);
    if (i) out += ',';
    out += "{\"tag\":"; append_json_tag (out, table_tags[i]);
    out += ",\"size\":" + std::to_string (hb_blob_get_length (table)) + "}";
    hb_blob_destroy (table);
  }
  out += ']';

  /* Layout scripts and languages, grouped by GSUB / GPOS. */
  out += ",\"scripts\":[";
  const hb_tag_t layout_tables[] = {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS};
  for (unsigned ti = 0; ti < 2; ti++)
  {
    if (ti) out += ',';
    out += "{\"table\":"; append_json_tag (out, layout_tables[ti]);
    out += ",\"items\":[";
    unsigned script_total = hb_ot_layout_table_get_script_tags (face, layout_tables[ti], 0, nullptr, nullptr);
    std::vector<hb_tag_t> script_tags (script_total);
    if (script_total) hb_ot_layout_table_get_script_tags (face, layout_tables[ti], 0, &script_total, script_tags.data ());
    for (unsigned si = 0; si < script_total; si++)
    {
      if (si) out += ',';
      hb_tag_t iso = script_tags[si] == HB_TAG('D','F','L','T')
                   ? HB_SCRIPT_COMMON
                   : hb_script_to_iso15924_tag (hb_ot_tag_to_script (script_tags[si]));
      out += "{\"tag\":"; append_json_tag (out, script_tags[si]);
      out += ",\"iso\":"; append_json_tag (out, iso);
      out += ",\"languages\":[";
      unsigned lang_total = hb_ot_layout_script_get_language_tags (face, layout_tables[ti], si, 0, nullptr, nullptr);
      std::vector<hb_tag_t> lang_tags (lang_total);
      if (lang_total) hb_ot_layout_script_get_language_tags (face, layout_tables[ti], si, 0, &lang_total, lang_tags.data ());
      for (unsigned li = 0; li < lang_total; li++)
      {
        if (li) out += ',';
        hb_language_t language = hb_ot_tag_to_language (lang_tags[li]);
        const char *language_s = hb_language_to_string (language);
        out += "{\"tag\":"; append_json_tag (out, lang_tags[li]);
        out += ",\"language\":"; append_json_string (out, language_s ? language_s : "", language_s ? strlen (language_s) : 0);
        out += '}';
      }
      out += "]}";
    }
    out += "]}";
  }
  out += ']';

  /* All table features, with duplicates removed per table like hb-info. */
  out += ",\"features\":[";
  for (unsigned ti = 0; ti < 2; ti++)
  {
    if (ti) out += ',';
    out += "{\"table\":"; append_json_tag (out, layout_tables[ti]);
    out += ",\"items\":[";
    unsigned feature_count = hb_ot_layout_table_get_feature_tags (face, layout_tables[ti], 0, nullptr, nullptr);
    std::vector<hb_tag_t> features (feature_count);
    if (feature_count) hb_ot_layout_table_get_feature_tags (face, layout_tables[ti], 0, &feature_count, features.data ());
    hb_set_t *seen = hb_set_create ();
    bool first_feature = true;
    for (unsigned fi = 0; fi < feature_count; fi++)
    {
      if (hb_set_has (seen, features[fi])) continue;
      hb_set_add (seen, features[fi]);
      hb_ot_name_id_t name_id = HB_OT_NAME_ID_INVALID;
      hb_ot_layout_feature_get_name_ids (face, layout_tables[ti], fi,
                                          &name_id, nullptr, nullptr, nullptr, nullptr);
      std::string name = name_id == HB_OT_NAME_ID_INVALID ? "" : font_name (face, name_id);
      if (!first_feature) out += ',';
      out += "{\"tag\":"; append_json_tag (out, features[fi]);
      out += ",\"name\":"; append_json_string (out, name.data (), name.size ());
      out += '}';
      first_feature = false;
    }
    hb_set_destroy (seen);
    out += "]}";
  }
  out += ']';

  /* Variation axes and named instances. */
  out += ",\"variations\":{\"axes\":[";
  unsigned axis_count = hb_ot_var_get_axis_infos (face, 0, nullptr, nullptr);
  std::vector<hb_ot_var_axis_info_t> axes (axis_count);
  if (axis_count) hb_ot_var_get_axis_infos (face, 0, &axis_count, axes.data ());
  for (unsigned i = 0; i < axis_count; i++)
  {
    if (i) out += ',';
    std::string name = font_name (face, axes[i].name_id);
    out += "{\"tag\":"; append_json_tag (out, axes[i].tag);
    out += ",\"name\":"; append_json_string (out, name.data (), name.size ());
    out += ",\"min\":"; append_json_float (out, axes[i].min_value);
    out += ",\"default\":"; append_json_float (out, axes[i].default_value);
    out += ",\"max\":"; append_json_float (out, axes[i].max_value);
    out += ",\"hidden\":" + std::string ((axes[i].flags & HB_OT_VAR_AXIS_FLAG_HIDDEN) ? "true" : "false") + "}";
  }
  out += "],\"instances\":[";
  unsigned instance_count = hb_ot_var_get_named_instance_count (face);
  for (unsigned i = 0; i < instance_count; i++)
  {
    if (i) out += ',';
    std::string name = font_name (face, hb_ot_var_named_instance_get_subfamily_name_id (face, i));
    unsigned coord_count = hb_ot_var_named_instance_get_design_coords (face, i, nullptr, nullptr);
    std::vector<float> coords (coord_count);
    if (coord_count) hb_ot_var_named_instance_get_design_coords (face, i, &coord_count, coords.data ());
    out += "{\"index\":" + std::to_string (i) + ",\"name\":";
    append_json_string (out, name.data (), name.size ());
    out += ",\"coords\":[";
    for (unsigned ci = 0; ci < coord_count; ci++) { if (ci) out += ','; append_json_float (out, coords[ci]); }
    out += "]}";
  }
  out += "]}";

  /* Palettes, their actual colors, and optional color names. */
  out += ",\"palettes\":[";
  unsigned palette_count = hb_ot_color_palette_get_count (face);
  for (unsigned pi = 0; pi < palette_count; pi++)
  {
    if (pi) out += ',';
    hb_ot_name_id_t palette_name_id = hb_ot_color_palette_get_name_id (face, pi);
    std::string name = palette_name_id == HB_OT_NAME_ID_INVALID ? "" : font_name (face, palette_name_id);
    out += "{\"index\":" + std::to_string (pi) + ",\"flags\":" +
           std::to_string ((unsigned) hb_ot_color_palette_get_flags (face, pi)) + ",\"name\":";
    append_json_string (out, name.data (), name.size ());
    out += ",\"colors\":[";
    unsigned color_count = hb_ot_color_palette_get_colors (face, pi, 0, nullptr, nullptr);
    std::vector<hb_color_t> colors (color_count);
    if (color_count) hb_ot_color_palette_get_colors (face, pi, 0, &color_count, colors.data ());
    for (unsigned ci = 0; ci < color_count; ci++)
    {
      if (ci) out += ',';
      hb_ot_name_id_t color_name_id = hb_ot_color_palette_color_get_name_id (face, ci);
      std::string color_name = color_name_id == HB_OT_NAME_ID_INVALID ? "" : font_name (face, color_name_id);
      char rgba[10];
      snprintf (rgba, sizeof rgba, "#%02X%02X%02X%02X",
                hb_color_get_red (colors[ci]), hb_color_get_green (colors[ci]),
                hb_color_get_blue (colors[ci]), hb_color_get_alpha (colors[ci]));
      out += "{\"index\":" + std::to_string (ci) + ",\"rgba\":";
      append_json_string (out, rgba, strlen (rgba));
      out += ",\"name\":"; append_json_string (out, color_name.data (), color_name.size ());
      out += '}';
    }
    out += "]}";
  }
  out += ']';

  out += ",\"meta\":[";
  unsigned meta_count = hb_ot_meta_get_entry_tags (face, 0, nullptr, nullptr);
  std::vector<hb_ot_meta_tag_t> meta_tags (meta_count);
  if (meta_count) hb_ot_meta_get_entry_tags (face, 0, &meta_count, meta_tags.data ());
  for (unsigned i = 0; i < meta_count; i++)
  {
    if (i) out += ',';
    hb_blob_t *entry = hb_ot_meta_reference_entry (face, meta_tags[i]);
    unsigned len = 0;
    const char *data = hb_blob_get_data (entry, &len);
    out += "{\"tag\":"; append_json_tag (out, meta_tags[i]);
    out += ",\"data\":"; append_json_string (out, data ? data : "", len);
    out += '}';
    hb_blob_destroy (entry);
  }
  out += "]}";

  hb_font_destroy (font);
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

static char *
font_info_grid (const uint8_t *font_bytes, unsigned font_len,
                bool characters, unsigned start, unsigned limit, const char *query, bool locate)
{
  if (!limit || limit > 128) limit = 64;
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("{\"total\":0,\"unfiltered_total\":0,\"items\":[],\"next\":0,\"more\":false}");
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);
  hb_font_t *font = hb_font_create (face);
  apply_variations (font);

  auto lowercase = [] (std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
  };
  std::string search = lowercase (query ? query : "");
  /* Character-map searches default to Unicode. Explicit glyph queries
   * find every nominal or variation mapping to that glyph. Preserve case
   * in names: A and a may name different glyphs. */
  bool search_by_name = characters && search.compare (0, 5, "name:") == 0;
  bool search_by_gid = characters && search.compare (0, 3, "gid") == 0;
  /* Single characters (including spaces and supplementary characters) are
   * literal. Longer queries accept hexadecimal code points or a sequence. */
  std::vector<hb_codepoint_t> codepoints;
  if (characters && !search.empty () && !search_by_name && !search_by_gid)
  {
    hb_buffer_t *buf = hb_buffer_create ();
    hb_buffer_add_utf8 (buf, query, -1, 0, -1);
    unsigned count;
    hb_glyph_info_t *infos = hb_buffer_get_glyph_infos (buf, &count);
    if (count == 1 || (count == 2 &&
        ((infos[1].codepoint >= 0xFE00 && infos[1].codepoint <= 0xFE0F) ||
         (infos[1].codepoint >= 0xE0100 && infos[1].codepoint <= 0xE01EF))))
      for (unsigned i = 0; i < count; i++) codepoints.push_back (infos[i].codepoint);
    hb_buffer_destroy (buf);
    if (codepoints.empty ())
    {
      const char *p = search.c_str ();
      while (*p)
      {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (!strncmp (p, "u+", 2) || !strncmp (p, "0x", 2)) p += 2;
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
        { codepoints.clear (); break; }
        char *end;
        unsigned long cp = strtoul (p, &end, 16);
        if (cp > 0x10FFFF || (*end && *end != ' ' && *end != ','))
        { codepoints.clear (); break; }
        codepoints.push_back ((hb_codepoint_t) cp);
        p = end;
      }
    }
  }
  hb_codepoint_t search_gid = HB_CODEPOINT_INVALID;
  if (search_by_name)
  {
    if (!hb_font_get_glyph_from_name (font, query + 5, -1, &search_gid))
      search_gid = HB_CODEPOINT_INVALID;
  }
  else if ((!characters || search_by_gid) && !search.empty ())
  {
    const char *p = search.c_str ();
    if (!strncmp (p, "gid", 3)) p += 3;
    if (*p >= '0' && *p <= '9')
    {
      char *end;
      unsigned long value = strtoul (p, &end, 10);
      if (!*end && value < HB_CODEPOINT_INVALID) search_gid = value;
    }
  }

  unsigned total = 0, unfiltered_total = 0;
  struct item_t { hb_codepoint_t gid, unicode, selector; unsigned index; };
  std::vector<item_t> page;
  auto consider = [&] (hb_codepoint_t gid, hb_codepoint_t unicode, hb_codepoint_t selector) {
    unfiltered_total++;
    if (!search.empty ())
    {
      if (search_by_name || search_by_gid)
      {
        if (search_gid == HB_CODEPOINT_INVALID || gid != search_gid) return;
      }
      else if (characters)
      {
        if (codepoints.empty () || codepoints.size () > 2 || codepoints[0] != unicode ||
            (codepoints.size () == 2 && codepoints[1] != selector)) return;
      }
      else if (search_gid != HB_CODEPOINT_INVALID)
      {
        if (gid != search_gid) return;
      }
      else
      {
        char name[128] = {0};
        hb_font_glyph_to_string (font, gid, name, sizeof name);
        if (lowercase (name).find (search) == std::string::npos) return;
      }
    }
    if (total++ >= start && page.size () < limit)
      page.push_back ({gid, unicode, selector, unfiltered_total - 1});
  };
  hb_set_t *unicodes = nullptr;
  hb_map_t *mapping = nullptr;
  hb_set_t *selectors = nullptr;
  if (characters)
  {
    unicodes = hb_set_create ();
    mapping = hb_map_create ();
    hb_face_collect_nominal_glyph_mapping (face, mapping, unicodes);
    for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next (unicodes, &u);)
      consider (hb_map_get (mapping, u), u, HB_CODEPOINT_INVALID);
    selectors = hb_set_create ();
    hb_face_collect_variation_selectors (face, selectors);
    hb_set_t *vars = hb_set_create ();
    for (hb_codepoint_t vs = HB_SET_VALUE_INVALID; hb_set_next (selectors, &vs);)
    {
      hb_set_clear (vars);
      hb_face_collect_variation_unicodes (face, vs, vars);
      for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next (vars, &u);)
      {
        hb_codepoint_t gid = 0;
        if (hb_font_get_variation_glyph (font, u, vs, &gid)) consider (gid, u, vs);
      }
    }
    hb_set_destroy (vars);
  }
  else
    for (unsigned gid = 0; gid < hb_face_get_glyph_count (face); gid++)
      consider (gid, HB_CODEPOINT_INVALID, HB_CODEPOINT_INVALID);

  std::string out = "{\"total\":" + std::to_string (total) +
                    ",\"unfiltered_total\":" + std::to_string (unfiltered_total) + ",\"items\":[";
  unsigned emitted = 0;
  auto append_item = [&] (const item_t &item) {
    if (emitted) out += ',';
    hb_codepoint_t gid = item.gid, unicode = item.unicode, selector = item.selector;
    char name[128] = {0};
    if (!hb_font_get_glyph_name (font, gid, name, sizeof name)) name[0] = '\0';
    /* Searches locate entries in the full grid; only visible grid pages
     * need artwork. In particular, a distant match must not draw everything
     * preceding it. */
    std::string svg = locate ? "" : glyph_svg (face, font, gid);
    out += "{\"index\":" + std::to_string (item.index) + ",\"gid\":" + std::to_string (gid);
    if (characters)
    {
      out += ",\"unicode\":" + std::to_string (unicode);
      if (selector != HB_CODEPOINT_INVALID)
        out += ",\"selector\":" + std::to_string (selector);
    }
    out += ",\"name\":"; append_json_string (out, name, strlen (name));
    out += ",\"svg\":"; append_json_string (out, svg.data (), svg.size ());
    out += '}';
    emitted++;
  };

  for (const auto &item : page) append_item (item);
  out += "],\"next\":" + std::to_string (start + emitted) +
         ",\"more\":" + std::string (start + emitted < total ? "true" : "false") + "}";

  hb_set_destroy (selectors);
  hb_map_destroy (mapping);
  hb_set_destroy (unicodes);
  hb_font_destroy (font);
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

EMSCRIPTEN_KEEPALIVE
char *web_font_info_unicodes (const uint8_t *font_bytes, unsigned font_len,
                              unsigned start, unsigned limit, const char *query, bool locate)
{
  return font_info_grid (font_bytes, font_len, true, start, limit, query, locate);
}

EMSCRIPTEN_KEEPALIVE
char *web_font_info_glyphs (const uint8_t *font_bytes, unsigned font_len,
                            unsigned start, unsigned limit, const char *query, bool locate)
{
  return font_info_grid (font_bytes, font_len, false, start, limit, query, locate);
}

/* JSON-describe the font's CPAL palettes (name, flags).
 * Returns "[]" for fonts without a CPAL table.  Each entry
 * carries the palette name (from name table, or empty if
 * unset) and the flags bitfield (1=light bg, 2=dark bg) so
 * the JS can fall back on those when there's no name.
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_font_palettes (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("[]");
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);

  unsigned n = hb_ot_color_palette_get_count (face);
  if (!n) { hb_face_destroy (face); return strdup ("[]"); }

  std::string out = "[";
  for (unsigned i = 0; i < n; i++)
  {
    char name[64] = {0};
    unsigned sz = 0;
    hb_ot_name_id_t nid = hb_ot_color_palette_get_name_id (face, i);
    if (nid != HB_OT_NAME_ID_INVALID)
    {
      sz = sizeof name;
      hb_ot_name_get_utf8 (face, nid, HB_LANGUAGE_INVALID, &sz, name);
    }
    unsigned flags = hb_ot_color_palette_get_flags (face, i);
    if (i) out += ',';
    out += "{\"name\":";
    append_json_string (out, name, sz);
    out += ",\"flags\":" + std::to_string (flags) + "}";
  }
  out += ']';
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

/* JSON-describe the font's fvar axes (tag, min, def, max,
 * name).  Returns "[]" for fonts without an fvar table.
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_font_axes (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("[]");
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);

  unsigned n = hb_ot_var_get_axis_count (face);
  if (!n) { hb_face_destroy (face); return strdup ("[]"); }

  hb_ot_var_axis_info_t axes[32];
  unsigned got = sizeof axes / sizeof axes[0];
  hb_ot_var_get_axis_infos (face, 0, &got, axes);

  std::string out = "[";
  bool first = true;
  for (unsigned i = 0; i < got; i++)
  {
    /* Skip hidden axes -- not meant for direct UI exposure. */
    if (axes[i].flags & HB_OT_VAR_AXIS_FLAG_HIDDEN) continue;
    char name[64] = {0};
    unsigned sz = sizeof name;
    hb_ot_name_get_utf8 (face, axes[i].name_id, HB_LANGUAGE_INVALID, &sz, name);
    if (!first) out += ',';
    out += "{\"tag\":";
    append_json_tag (out, axes[i].tag);
    char values[96];
    snprintf (values, sizeof values,
              ",\"min\":%g,\"def\":%g,\"max\":%g,\"name\":",
              axes[i].min_value, axes[i].default_value, axes[i].max_value);
    out += values;
    append_json_string (out, name, sz);
    out += '}';
    first = false;
  }
  out += ']';
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

/* JSON-describe the font's GSUB+GPOS layout features for the
 * script detected from @utf8_text.  Each entry carries the
 * four-character tag and an optional name (from the name table,
 * for stylistic sets / character variants).  Deduplicates
 * across GSUB and GPOS.
 * Returns "[]" for fonts with no features.
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_font_features (const uint8_t *font_bytes, unsigned font_len,
                         const char *utf8_text)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("[]");
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);

  /* Guess the script from the text. */
  hb_buffer_t *buf = hb_buffer_create ();
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_script_t script = hb_buffer_get_script (buf);
  hb_buffer_destroy (buf);

  hb_tag_t script_tags[2];
  unsigned script_count = 2;
  hb_ot_tags_from_script_and_language (script, HB_LANGUAGE_INVALID,
                                      &script_count, script_tags,
                                      nullptr, nullptr);

  /* Collect feature tags from both GSUB and GPOS for any
   * matching script.  Use a simple linear scan to dedup. */
  hb_tag_t tags[256];
  unsigned n_tags = 0;

  hb_tag_t tables[] = { HB_OT_TAG_GSUB, HB_OT_TAG_GPOS };
  for (auto table_tag : tables)
  {
    unsigned script_idx;
    if (!hb_ot_layout_table_select_script (face, table_tag,
                                           script_count, script_tags,
                                           &script_idx, nullptr))
      continue;

    unsigned lang_idx = HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX;

    unsigned feat_count = 256;
    hb_tag_t feat_tags[256];
    hb_ot_layout_language_get_feature_tags (face, table_tag,
                                            script_idx, lang_idx,
                                            0, &feat_count, feat_tags);
    for (unsigned i = 0; i < feat_count && n_tags < 256; i++)
    {
      bool dup = false;
      for (unsigned j = 0; j < n_tags; j++)
        if (tags[j] == feat_tags[i]) { dup = true; break; }
      if (!dup) tags[n_tags++] = feat_tags[i];
    }
  }

  if (!n_tags) { hb_face_destroy (face); return strdup ("[]"); }

  std::string out = "[";
  for (unsigned i = 0; i < n_tags; i++)
  {
    /* Try to get a human-readable name for ss01-ss20, cv01-cv99. */
    char name[128] = {0};
    unsigned sz = 0;
    unsigned feat_idx;
    if (hb_ot_layout_language_find_feature (face, HB_OT_TAG_GSUB,
                                            0, HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                            tags[i], &feat_idx))
    {
      hb_ot_name_id_t name_id;
      if (hb_ot_layout_feature_get_name_ids (face, HB_OT_TAG_GSUB,
                                             feat_idx,
                                             &name_id, nullptr,
                                             nullptr, nullptr, nullptr))
      {
        sz = sizeof name;
        hb_ot_name_get_utf8 (face, name_id, HB_LANGUAGE_INVALID, &sz, name);
      }
    }

    if (i) out += ',';
    out += "{\"tag\":";
    append_json_tag (out, tags[i]);
    out += ",\"name\":";
    append_json_string (out, name, sz);
    out += '}';
  }
  out += ']';
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

EMSCRIPTEN_KEEPALIVE
const char *web_hb_version ()
{
  return hb_version_string ();
}

EMSCRIPTEN_KEEPALIVE
const char *web_hb_revision ()
{
#ifdef WEB_HB_REVISION
  return WEB_HB_REVISION;
#else
  return "";
#endif
}

/* Returns true if @utf8_text contains codepoints from more than
 * one Unicode script (ignoring Common and Inherited). */
EMSCRIPTEN_KEEPALIVE
int web_is_multi_script (const char *utf8_text)
{
  hb_buffer_t *buf = hb_buffer_create ();
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, nullptr);

  hb_unicode_funcs_t *uf = hb_unicode_funcs_get_default ();
  hb_script_t first = HB_SCRIPT_INVALID;
  bool multi = false;
  for (unsigned i = 0; i < len; i++)
  {
    hb_script_t s = hb_unicode_script (uf, info[i].codepoint);
    if (s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED)
      continue;
    if (first == HB_SCRIPT_INVALID)
      first = s;
    else if (s != first)
    {
      multi = true;
      break;
    }
  }
  hb_buffer_destroy (buf);
  return multi;
}

/* Common: produce a shaped buffer for (font_bytes, text). */
static hb_buffer_t *
shape (const uint8_t *font_bytes, unsigned font_len,
       const char *utf8_text,
       hb_face_t **out_face, hb_font_t **out_font)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return nullptr;
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);
  hb_font_t *font = hb_font_create (face);

  apply_variations (font);

  hb_buffer_t *buf = hb_buffer_create ();
  hb_buffer_set_cluster_level (buf, (hb_buffer_cluster_level_t) g_cluster_level);
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, g_feature_list, g_feature_count);

  *out_face = face;
  *out_font = font;
  return buf;
}

/* Returns a malloc'd JSON string of the shaped glyph stream:
 *   [{"gid":N,"cluster":N,"x_offset":N,"y_offset":N,"x_advance":N,"y_advance":N},...]
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_shape_json (const uint8_t *font_bytes, unsigned font_len,
                      const char *utf8_text,
                      float font_size_px)
{
  hb_face_t *face = nullptr;
  hb_font_t *font = nullptr;
  hb_buffer_t *buf = shape (font_bytes, font_len, utf8_text, &face, &font);
  if (!buf)
    return strdup ("[]");

  int scale = (int) (font_size_px * 64.f);
  hb_font_set_scale (font, scale, scale);
  hb_buffer_clear_contents (buf);
  hb_buffer_set_cluster_level (buf, (hb_buffer_cluster_level_t) g_cluster_level);
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, g_feature_list, g_feature_count);

  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, nullptr);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, nullptr);
  const float div = 64.f;

  std::string out = "[";
  for (unsigned i = 0; i < len; i++)
  {
    char name[64] = {0};
    hb_font_glyph_to_string (font, info[i].codepoint, name, sizeof name);
    if (i) out += ',';
    out += "{\"gid\":" + std::to_string (info[i].codepoint) + ",\"name\":";
    append_json_string (out, name, strlen (name));
    char values[192];
    snprintf (values, sizeof values,
              ",\"cluster\":%u,\"x_offset\":%.2f,\"y_offset\":%.2f,"
              "\"x_advance\":%.2f,\"y_advance\":%.2f}",
              info[i].cluster,
              pos[i].x_offset / div, pos[i].y_offset / div,
              pos[i].x_advance / div, pos[i].y_advance / div);
    out += values;
  }
  out += ']';

  hb_buffer_destroy (buf);
  hb_font_destroy (font);
  hb_face_destroy (face);
  return strdup (out.c_str ());
}

/* Render shaped text via hb-vector in the requested format.
 *
 * @format: HB_VECTOR_FORMAT_SVG or HB_VECTOR_FORMAT_PDF.
 * @out_len: out-param receiving the byte length of the result
 *           (excluding the trailing NUL).  Pass NULL to skip.
 * Returns a malloc'd buffer with the rendered output plus a
 * trailing NUL.  Caller frees with web_free_string(). */
static char *
render (hb_vector_format_t format,
        const uint8_t *font_bytes, unsigned font_len,
        const char *utf8_text,
        float font_size_px,
        unsigned *out_len)
{
  if (out_len) *out_len = 0;

  hb_face_t *face = nullptr;
  hb_font_t *font = nullptr;
  hb_buffer_t *buf = shape (font_bytes, font_len, utf8_text, &face, &font);
  if (!buf)
    return strdup ("");

  /* Shape at pixel * SCALE for sub-pixel precision; the
   * render contexts below divide by SCALE on emit to land on
   * pixels in the produced SVG/PDF. */
  int fsp = (int) (font_size_px * (float) SCALE);
  hb_font_set_scale (font, fsp, fsp);
  hb_buffer_clear_contents (buf);
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, g_feature_list, g_feature_count);

  /* Route mono fonts to vector_draw and color fonts to
   * vector_paint.  TODO: collapse once HarfBuzz exposes a paint
   * entry point that handles both (planned for the next
   * release). */
  hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                       hb_ot_color_has_layers (face) ||
                       hb_ot_color_has_png (face);

  hb_vector_paint_t *p = nullptr;
  hb_vector_draw_t  *d = nullptr;
  if (is_color)
  {
    p = hb_vector_paint_create_or_fail (format);
    if (p)
    {
      hb_vector_paint_set_palette (p, g_palette);
      hb_vector_paint_set_scale_factor (p, (float) SCALE, (float) SCALE);
      hb_vector_paint_set_foreground (p, g_foreground);
      hb_vector_paint_set_background (p, g_background);
    }
  }
  else
  {
    d = hb_vector_draw_create_or_fail (format);
    if (d)
    {
      hb_vector_draw_set_scale_factor (d, (float) SCALE, (float) SCALE);
      hb_vector_draw_set_foreground (d, g_foreground);
      hb_vector_draw_set_background (d, g_background);
    }
  }

  /* Namespace SVG ids per render so multiple hb-vector
   * SVGs embedded in the same page (shape tab vs vector
   * tab) can't collide on short IDs like "c0" / "gr0". */
  if (format == HB_VECTOR_FORMAT_SVG)
  {
    static unsigned s_counter = 0;
    char pfx[16];
    snprintf (pfx, sizeof pfx, "v%u-", ++s_counter);
    if (p) hb_vector_paint_set_svg_prefix (p, pfx);
  }

  if (!p && !d)
  {
    hb_buffer_destroy (buf);
    hb_font_destroy (font);
    hb_face_destroy (face);
    return strdup ("");
  }

  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, nullptr);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, nullptr);

  /* Seed extents with the logical line box so the viewBox covers
   * the typographic rectangle (advance × ascender+descender),
   * not just glyph ink.  Per-glyph EXPAND then unions ink that
   * overshoots — italic LSBs, accents, deep descenders.
   *
   * hb-vector stores extents in Y-up (font) space; bottom edge
   * is at descender (negative), top at ascender (positive). */
  float total_x = 0.f;
  for (unsigned i = 0; i < len; i++) total_x += pos[i].x_advance;
  hb_font_extents_t fe;
  hb_font_get_h_extents (font, &fe);
  float asc = (float) fe.ascender;   /* positive */
  float desc = (float) fe.descender; /* negative */
  /* Advances and h_extents are in input space (pixel*SCALE);
   * set_extents divides by the context's scale_factor, so we
   * pass them through without pre-scaling. */
  hb_vector_extents_t logical = { 0.f, asc, total_x, desc - asc };
  if (p) hb_vector_paint_set_extents (p, &logical);
  else   hb_vector_draw_set_extents  (d, &logical);

  float pen_x = 0.f;
  float pen_y = 0.f;
  for (unsigned i = 0; i < len; i++)
  {
    float gx = pen_x + pos[i].x_offset;
    float gy = pen_y + pos[i].y_offset;
    if (p)
    {
      hb_vector_paint_set_transform (p, 1.f, 0.f, 0.f, 1.f, gx, gy);
      hb_vector_paint_glyph (p, font, info[i].codepoint,
                             HB_VECTOR_EXTENTS_MODE_EXPAND);
    }
    else
    {
      hb_vector_draw_set_transform (d, 1.f, 0.f, 0.f, 1.f, gx, gy);
      hb_vector_draw_glyph (d, font, info[i].codepoint,
                            HB_VECTOR_EXTENTS_MODE_EXPAND);
    }
    pen_x += pos[i].x_advance;
    pen_y += pos[i].y_advance;
  }

  hb_blob_t *out = p ? hb_vector_paint_render (p)
                     : hb_vector_draw_render  (d);
  unsigned blob_len = 0;
  const char *out_data = hb_blob_get_data (out, &blob_len);

  char *str = (char *) malloc ((size_t) blob_len + 1);
  memcpy (str, out_data, blob_len);
  str[blob_len] = '\0';
  if (out_len) *out_len = blob_len;

  hb_blob_destroy (out);
  hb_vector_paint_destroy (p);
  hb_vector_draw_destroy (d);
  hb_buffer_destroy (buf);
  hb_font_destroy (font);
  hb_face_destroy (face);
  return str;
}

EMSCRIPTEN_KEEPALIVE
char *web_render_svg (const uint8_t *font_bytes, unsigned font_len,
                      const char *utf8_text,
                      float font_size_px)
{
  return render (HB_VECTOR_FORMAT_SVG, font_bytes, font_len,
                 utf8_text, font_size_px, nullptr);
}

EMSCRIPTEN_KEEPALIVE
char *web_render_pdf (const uint8_t *font_bytes, unsigned font_len,
                      const char *utf8_text,
                      float font_size_px,
                      unsigned *out_len)
{
  return render (HB_VECTOR_FORMAT_PDF, font_bytes, font_len,
                 utf8_text, font_size_px, out_len);
}

/* Subset @font_bytes to the codepoints in @utf8_text and
 * return the resulting font as a malloc'd byte buffer.
 * @out_len receives the buffer's byte length.
 * Returns NULL on failure (invalid font, OOM, hb_subset_or_fail).
 * Caller frees the buffer with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
uint8_t *web_subset (const uint8_t *font_bytes, unsigned font_len,
                     const char *utf8_text,
                     unsigned *out_len)
{
  if (out_len) *out_len = 0;

  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return nullptr;
  hb_face_t *face = hb_face_create (blob, g_face_index);
  hb_blob_destroy (blob);

  hb_subset_input_t *input = hb_subset_input_create_or_fail ();
  if (!input)
  {
    hb_face_destroy (face);
    return nullptr;
  }

  /* Pin each fvar axis at its current g_variations value
   * so the subset comes out as a static instance.  Axes not
   * mentioned in g_variations are pinned at their default. */
  if (g_subset_instantiate)
  {
    unsigned axis_count = hb_ot_var_get_axis_count (face);
    if (axis_count)
    {
      hb_ot_var_axis_info_t axes[32];
      unsigned got = sizeof axes / sizeof axes[0];
      hb_ot_var_get_axis_infos (face, 0, &got, axes);
      for (unsigned i = 0; i < got; i++)
      {
        float value = axes[i].default_value;
        /* Scan g_variations for "tag=value" matching this axis. */
        const char *p = g_variations;
        while (p && *p)
        {
          const char *end = strchr (p, ',');
          int len = end ? (int) (end - p) : (int) strlen (p);
          hb_variation_t v;
          if (hb_variation_from_string (p, len, &v) && v.tag == axes[i].tag)
          {
            value = v.value;
            break;
          }
          p = end ? end + 1 : nullptr;
        }
        hb_subset_input_pin_axis_location (input, face, axes[i].tag, value);
      }
    }
  }

  /* Apply feature settings to the subset input: add explicitly
   * enabled features to the retained set; remove explicitly
   * disabled features. */
  hb_set_t *feat_set = hb_subset_input_set (input, HB_SUBSET_SETS_LAYOUT_FEATURE_TAG);
  for (unsigned i = 0; i < g_feature_count; i++)
  {
    if (g_feature_list[i].value)
      hb_set_add (feat_set, g_feature_list[i].tag);
    else
      hb_set_del (feat_set, g_feature_list[i].tag);
  }

  /* Add every Unicode codepoint in the text to the subset's
   * unicode set.  hb-subset closes over GSUB/GPOS lookups
   * and pulls in any glyphs needed to shape that input. */
  hb_set_t *unicodes = hb_subset_input_unicode_set (input);
  hb_buffer_t *buf = hb_buffer_create ();
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  unsigned n = hb_buffer_get_length (buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, nullptr);
  for (unsigned i = 0; i < n; i++)
    hb_set_add (unicodes, info[i].codepoint);
  hb_buffer_destroy (buf);

  hb_face_t *subset_face = hb_subset_or_fail (face, input);
  hb_subset_input_destroy (input);
  hb_face_destroy (face);
  if (!subset_face) return nullptr;

  hb_blob_t *subset_blob = hb_face_reference_blob (subset_face);
  hb_face_destroy (subset_face);
  if (!subset_blob) return nullptr;

  unsigned blob_len = 0;
  const char *src = hb_blob_get_data (subset_blob, &blob_len);
  uint8_t *out = (uint8_t *) malloc (blob_len);
  if (out)
  {
    memcpy (out, src, blob_len);
    if (out_len) *out_len = blob_len;
  }
  hb_blob_destroy (subset_blob);
  return out;
}


/* Render shaped text via hb-raster and return a BGRA32 pixel
 * buffer.  *out_width / *out_height receive the buffer's pixel
 * dimensions.  Caller frees the returned buffer with
 * web_free_string(). */
EMSCRIPTEN_KEEPALIVE
uint8_t *web_render_raster (const uint8_t *font_bytes, unsigned font_len,
                            const char *utf8_text,
                            float font_size_px,
                            unsigned *out_width,
                            unsigned *out_height)
{
  if (out_width)  *out_width  = 0;
  if (out_height) *out_height = 0;

  hb_face_t *face = nullptr;
  hb_font_t *font = nullptr;
  hb_buffer_t *buf = shape (font_bytes, font_len, utf8_text, &face, &font);
  if (!buf) return nullptr;

  /* Re-shape at pixel*SCALE for sub-pixel shaping precision;
   * the raster context below divides by SCALE on render. */
  int fsp = (int) (font_size_px * (float) SCALE);
  hb_font_set_scale (font, fsp, fsp);
  hb_buffer_clear_contents (buf);
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, g_feature_list, g_feature_count);

  /* Seed extents with the logical line box (advance × ascender+descender)
   * in pixel*SCALE Y-up units, then union with each glyph's translated ink
   * box so descender ink and italic LSBs aren't clipped — matching the
   * EXPAND mode the vector path uses. */
  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t    *info = hb_buffer_get_glyph_infos (buf, nullptr);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, nullptr);
  hb_font_extents_t fe;
  hb_font_get_h_extents (font, &fe);
  float ascent  = (float) fe.ascender;   /* positive */
  float descent = (float) -fe.descender; /* positive */
  float total_x = 0.f;
  for (unsigned i = 0; i < len; i++) total_x += pos[i].x_advance;

  float box_x_min = 0.f,        box_x_max = total_x;
  float box_y_min = -descent,   box_y_max = ascent;
  {
    float pen_x = 0.f, pen_y = 0.f;
    for (unsigned i = 0; i < len; i++)
    {
      hb_glyph_extents_t ge;
      if (hb_font_get_glyph_extents (font, info[i].codepoint, &ge))
      {
        float gx = pen_x + (float) pos[i].x_offset;
        float gy = pen_y + (float) pos[i].y_offset;
        float ix1 = gx + (float) ge.x_bearing;
        float ix2 = ix1 + (float) ge.width;
        float iy2 = gy + (float) ge.y_bearing;       /* top in Y-up */
        float iy1 = iy2 + (float) ge.height;         /* height is negative */
        if (ix1 < box_x_min) box_x_min = ix1;
        if (ix2 > box_x_max) box_x_max = ix2;
        if (iy1 < box_y_min) box_y_min = iy1;
        if (iy2 > box_y_max) box_y_max = iy2;
      }
      pen_x += (float) pos[i].x_advance;
      pen_y += (float) pos[i].y_advance;
    }
  }

  const float inv_scale = 1.f / (float) SCALE;
  int ext_x = (int) floorf (box_x_min * inv_scale);
  int ext_y = (int) floorf (box_y_min * inv_scale);
  int ext_x2 = (int) ceilf  (box_x_max * inv_scale);
  int ext_y2 = (int) ceilf  (box_y_max * inv_scale);
  unsigned w = (unsigned) (ext_x2 - ext_x);
  unsigned h = (unsigned) (ext_y2 - ext_y);
  if (!w || !h)
  {
    hb_buffer_destroy (buf);
    hb_font_destroy (font);
    hb_face_destroy (face);
    return nullptr;
  }

  hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                       hb_ot_color_has_layers (face) ||
                       hb_ot_color_has_png (face);

  hb_raster_paint_t *p = nullptr;
  hb_raster_draw_t  *d = nullptr;
  if (is_color)
  {
    p = hb_raster_paint_create_or_fail ();
    if (p)
    {
      hb_raster_paint_set_palette (p, g_palette);
      hb_raster_paint_set_foreground (p, g_foreground);
    }
  }
  else
    d = hb_raster_draw_create_or_fail ();

  /* Extents in pixel space, Y-up: ext_y is the bottom edge of the
   * unioned box (negative for typical horizontal text since the
   * baseline sits above the descender ink). */
  unsigned stride = w * 4;
  hb_raster_extents_t ext = { ext_x, ext_y, w, h, stride };

  uint8_t *out = (p || d) ? (uint8_t *) malloc ((size_t) stride * h)
                          : nullptr;
  if (out)
  {
    /* Pre-fill with background color (BGRA). */
    uint32_t bg_px = (uint32_t) hb_color_get_blue (g_background)
                   | ((uint32_t) hb_color_get_green (g_background) << 8)
                   | ((uint32_t) hb_color_get_red (g_background) << 16)
                   | ((uint32_t) hb_color_get_alpha (g_background) << 24);
    uint32_t *px = (uint32_t *) out;
    for (size_t i = 0; i < (size_t) w * h; i++) px[i] = bg_px;
  }
  if (!out)
  {
    hb_raster_paint_destroy (p);
    hb_raster_draw_destroy (d);
    hb_buffer_destroy (buf);
    hb_font_destroy (font);
    hb_face_destroy (face);
    return nullptr;
  }

  /* Per-glyph render + SRC_OVER composite onto out[].
   * Color path: paint returns BGRA32 premultiplied.
   * Mono path: draw returns A8 coverage; composite as black. */
  float pen_x = 0.f, pen_y = 0.f;
  for (unsigned i = 0; i < len; i++)
  {
    float gx = pen_x + pos[i].x_offset;
    float gy = pen_y + pos[i].y_offset;
    pen_x += pos[i].x_advance;
    pen_y += pos[i].y_advance;

    hb_raster_image_t *img;
    if (p)
    {
      hb_raster_paint_set_extents (p, &ext);
      hb_raster_paint_set_scale_factor (p, (float) SCALE, (float) SCALE);
      hb_raster_paint_set_transform (p, 1.f, 0.f, 0.f, 1.f, gx, gy);
      hb_raster_paint_glyph (p, font, info[i].codepoint);
      img = hb_raster_paint_render (p);
    }
    else
    {
      hb_raster_draw_reset (d);
      hb_raster_draw_set_extents (d, &ext);
      hb_raster_draw_set_scale_factor (d, (float) SCALE, (float) SCALE);
      hb_raster_draw_set_transform (d, 1.f, 0.f, 0.f, 1.f, gx, gy);
      hb_raster_draw_glyph (d, font, info[i].codepoint);
      img = hb_raster_draw_render (d);
    }
    if (!img) continue;

    const uint8_t *src = hb_raster_image_get_buffer (img);
    hb_raster_extents_t ie;
    hb_raster_image_get_extents (img, &ie);

    /* hb-raster buffer is Y-up (row 0 = bottom); canvas expects
     * Y-down (row 0 = top), so flip src row y to out row (h-1-y). */
    if (p)
    {
      for (unsigned y = 0; y < h; y++)
      {
        unsigned dy = h - 1 - y;
        for (unsigned x = 0; x < w; x++)
        {
          uint32_t s;
          memcpy (&s, src + y * ie.stride + x * 4, 4);
          if (!s) continue;
          uint8_t sa = (uint8_t) (s >> 24);
          uint32_t dpx;
          memcpy (&dpx, out + dy * stride + x * 4, 4);
          if (sa == 255) { dpx = s; }
          else
          {
            unsigned inv = 255 - sa;
            uint8_t rb = (uint8_t) (((dpx & 0xFF) * inv + 127) / 255) + (uint8_t) (s & 0xFF);
            uint8_t rg = (uint8_t) ((((dpx >> 8) & 0xFF) * inv + 127) / 255) + (uint8_t) ((s >> 8) & 0xFF);
            uint8_t rr = (uint8_t) ((((dpx >> 16) & 0xFF) * inv + 127) / 255) + (uint8_t) ((s >> 16) & 0xFF);
            uint8_t ra = (uint8_t) ((((dpx >> 24) & 0xFF) * inv + 127) / 255) + sa;
            dpx = (uint32_t) rb | ((uint32_t) rg << 8) | ((uint32_t) rr << 16) | ((uint32_t) ra << 24);
          }
          memcpy (out + dy * stride + x * 4, &dpx, 4);
        }
      }
      hb_raster_paint_recycle_image (p, img);
    }
    else
    {
      for (unsigned y = 0; y < h; y++)
      {
        unsigned dy = h - 1 - y;
        for (unsigned x = 0; x < w; x++)
        {
          uint8_t cov = src[y * ie.stride + x];
          if (!cov) continue;
          uint32_t dpx;
          memcpy (&dpx, out + dy * stride + x * 4, 4);
          uint8_t fr = hb_color_get_red (g_foreground);
          uint8_t fg = hb_color_get_green (g_foreground);
          uint8_t fb = hb_color_get_blue (g_foreground);
          if (cov == 255) {
            dpx = (uint32_t) fb | ((uint32_t) fg << 8) | ((uint32_t) fr << 16) | 0xFF000000u;
          } else {
            unsigned inv = 255 - cov;
            uint8_t rb = (uint8_t) (((dpx & 0xFF) * inv + 127) / 255 + fb * cov / 255);
            uint8_t rg = (uint8_t) ((((dpx >> 8) & 0xFF) * inv + 127) / 255 + fg * cov / 255);
            uint8_t rr = (uint8_t) ((((dpx >> 16) & 0xFF) * inv + 127) / 255 + fr * cov / 255);
            uint8_t ra = (uint8_t) ((((dpx >> 24) & 0xFF) * inv + 127) / 255) + cov;
            dpx = (uint32_t) rb | ((uint32_t) rg << 8) | ((uint32_t) rr << 16) | ((uint32_t) ra << 24);
          }
          memcpy (out + dy * stride + x * 4, &dpx, 4);
        }
      }
      hb_raster_draw_recycle_image (d, img);
    }
  }

  if (out_width)  *out_width  = w;
  if (out_height) *out_height = h;

  hb_raster_paint_destroy (p);
  hb_raster_draw_destroy (d);
  hb_buffer_destroy (buf);
  hb_font_destroy (font);
  hb_face_destroy (face);
  return out;
}

} /* extern "C" */
