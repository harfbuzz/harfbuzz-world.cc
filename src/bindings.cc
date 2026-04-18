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

/* Sub-pixel precision for shaped glyph positions.  Same
 * 26.6-fixed-point convention as the in-tree hb-vector /
 * hb-raster utils (and FreeType): shape at font_size * SCALE,
 * then tell the render context to divide by SCALE to land on
 * pixels. */
#define SUBPIXEL_BITS 6
#define SCALE (1 << SUBPIXEL_BITS)

extern "C" {

EMSCRIPTEN_KEEPALIVE
void web_free_string (char *s)
{
  free (s);
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
  hb_face_t *face = hb_face_create (blob, 0);
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
char *web_font_stats (const uint8_t *font_bytes, unsigned font_len)
{
  hb_blob_t *blob = hb_blob_create_or_fail ((const char *) font_bytes,
                                             font_len,
                                             HB_MEMORY_MODE_READONLY,
                                             nullptr, nullptr);
  if (!blob) return strdup ("{\"num_glyphs\":0,\"num_unicodes\":0,\"tables\":[]}");
  hb_face_t *face = hb_face_create (blob, 0);
  hb_blob_destroy (blob);

  unsigned num_glyphs = hb_face_get_glyph_count (face);
  hb_set_t *unicodes = hb_set_create ();
  hb_face_collect_unicodes (face, unicodes);
  unsigned num_unicodes = hb_set_get_population (unicodes);
  hb_set_destroy (unicodes);

  unsigned table_count = hb_face_get_table_tags (face, 0, nullptr, nullptr);
  hb_tag_t *tags = (hb_tag_t *) calloc (table_count ? table_count : 1, sizeof (hb_tag_t));
  if (table_count) hb_face_get_table_tags (face, 0, &table_count, tags);

  size_t cap = 64 + (size_t) table_count * 48 + 1;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off,
                   "{\"num_glyphs\":%u,\"num_unicodes\":%u,\"tables\":[",
                   num_glyphs, num_unicodes);
  for (unsigned i = 0; i < table_count; i++)
  {
    hb_blob_t *t = hb_face_reference_table (face, tags[i]);
    unsigned len = hb_blob_get_length (t);
    hb_blob_destroy (t);
    char tag[5] = {
      (char) ((tags[i] >> 24) & 0xff),
      (char) ((tags[i] >> 16) & 0xff),
      (char) ((tags[i] >>  8) & 0xff),
      (char) ( tags[i]        & 0xff),
      0
    };
    /* Tag chars are guaranteed printable ASCII per spec; no
     * escape needed for the JSON-significant ones since they
     * can't appear here. */
    off += snprintf (out + off, cap - off,
                     "%s{\"tag\":\"%s\",\"size\":%u}",
                     i ? "," : "", tag, len);
  }
  off += snprintf (out + off, cap - off, "]}");

  free (tags);
  hb_face_destroy (face);
  return out;
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
  hb_face_t *face = hb_face_create (blob, 0);
  hb_blob_destroy (blob);

  unsigned n = hb_ot_color_palette_get_count (face);
  if (!n) { hb_face_destroy (face); return strdup ("[]"); }

  size_t cap = 8 + (size_t) n * 96 + 1;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off, "[");
  for (unsigned i = 0; i < n; i++)
  {
    char name[64] = {0};
    unsigned sz = sizeof name;
    hb_ot_name_id_t nid = hb_ot_color_palette_get_name_id (face, i);
    if (nid != HB_OT_NAME_ID_INVALID)
      hb_ot_name_get_utf8 (face, nid, HB_LANGUAGE_INVALID, &sz, name);
    /* Escape the few JSON-significant chars we might see in
     * a CPAL name (quote, backslash).  Names rarely contain
     * control chars; if they do we just pass them through. */
    char esc[128];
    size_t eo = 0;
    for (const char *p = name; *p && eo + 2 < sizeof esc; p++)
    {
      if (*p == '"' || *p == '\\') esc[eo++] = '\\';
      esc[eo++] = *p;
    }
    esc[eo] = 0;
    unsigned flags = hb_ot_color_palette_get_flags (face, i);
    off += snprintf (out + off, cap - off,
                     "%s{\"name\":\"%s\",\"flags\":%u}",
                     i ? "," : "", esc, flags);
  }
  off += snprintf (out + off, cap - off, "]");
  hb_face_destroy (face);
  return out;
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
  hb_face_t *face = hb_face_create (blob, 0);
  hb_blob_destroy (blob);

  unsigned n = hb_ot_var_get_axis_count (face);
  if (!n) { hb_face_destroy (face); return strdup ("[]"); }

  hb_ot_var_axis_info_t axes[32];
  unsigned got = sizeof axes / sizeof axes[0];
  hb_ot_var_get_axis_infos (face, 0, &got, axes);

  size_t cap = 64 + 160 * got + 1;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off, "[");
  bool first = true;
  for (unsigned i = 0; i < got; i++)
  {
    /* Skip hidden axes -- not meant for direct UI exposure. */
    if (axes[i].flags & HB_OT_VAR_AXIS_FLAG_HIDDEN) continue;
    char name[64] = {0};
    unsigned sz = sizeof name;
    hb_ot_name_get_utf8 (face, axes[i].name_id, HB_LANGUAGE_INVALID, &sz, name);
    char tag[5] = {
      (char) ((axes[i].tag >> 24) & 0xff),
      (char) ((axes[i].tag >> 16) & 0xff),
      (char) ((axes[i].tag >> 8) & 0xff),
      (char) (axes[i].tag & 0xff),
      0
    };
    off += snprintf (out + off, cap - off,
                     "%s{\"tag\":\"%s\",\"min\":%g,\"def\":%g,\"max\":%g,\"name\":\"%s\"}",
                     first ? "" : ",", tag,
                     axes[i].min_value, axes[i].default_value, axes[i].max_value,
                     name);
    first = false;
  }
  off += snprintf (out + off, cap - off, "]");
  hb_face_destroy (face);
  return out;
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
  hb_face_t *face = hb_face_create (blob, 0);
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

  size_t cap = 64 + 128 * n_tags;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off, "[");
  for (unsigned i = 0; i < n_tags; i++)
  {
    char tag[5] = {
      (char) ((tags[i] >> 24) & 0xff),
      (char) ((tags[i] >> 16) & 0xff),
      (char) ((tags[i] >> 8) & 0xff),
      (char) (tags[i] & 0xff),
      0
    };

    /* Try to get a human-readable name for ss01-ss20, cv01-cv99. */
    char name[128] = {0};
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
        unsigned sz = sizeof name;
        hb_ot_name_get_utf8 (face, name_id, HB_LANGUAGE_INVALID, &sz, name);
      }
    }

    /* Escape the name for JSON. */
    char esc[256] = {0};
    unsigned eo = 0;
    for (const char *p = name; *p && eo + 2 < sizeof esc; p++)
    {
      if (*p == '"' || *p == '\\') esc[eo++] = '\\';
      esc[eo++] = *p;
    }
    esc[eo] = 0;

    off += snprintf (out + off, cap - off,
                     "%s{\"tag\":\"%s\",\"name\":\"%s\"}",
                     i ? "," : "", tag, esc);
  }
  off += snprintf (out + off, cap - off, "]");
  hb_face_destroy (face);
  return out;
}

EMSCRIPTEN_KEEPALIVE
const char *web_hb_version ()
{
  return hb_version_string ();
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
  hb_face_t *face = hb_face_create (blob, 0);
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

  /* Estimate JSON capacity:  ~160 bytes per glyph entry leaves
   * room for a 64-char name on top of the numeric fields. */
  size_t cap = 16 + 160 * (size_t) len + 1;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off, "[");
  for (unsigned i = 0; i < len; i++)
  {
    char name[64] = {0};
    hb_font_glyph_to_string (font, info[i].codepoint, name, sizeof name);
    /* Escape the few JSON-significant chars that can show up in
     * glyph names (quote, backslash). */
    char esc[128];
    size_t eo = 0;
    for (const char *p = name; *p && eo + 2 < sizeof esc; p++)
    {
      if (*p == '"' || *p == '\\') esc[eo++] = '\\';
      esc[eo++] = *p;
    }
    esc[eo] = 0;
    off += snprintf (out + off, cap - off,
                     "%s{\"gid\":%u,\"name\":\"%s\",\"cluster\":%u,"
                     "\"x_offset\":%.2f,\"y_offset\":%.2f,"
                     "\"x_advance\":%.2f,\"y_advance\":%.2f}",
                     i ? "," : "",
                     info[i].codepoint, esc, info[i].cluster,
                     pos[i].x_offset / div, pos[i].y_offset / div,
                     pos[i].x_advance / div, pos[i].y_advance / div);
  }
  off += snprintf (out + off, cap - off, "]");

  hb_buffer_destroy (buf);
  hb_font_destroy (font);
  hb_face_destroy (face);
  return out;
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
    if (d) hb_vector_draw_set_svg_prefix  (d, pfx);
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
   * hb-vector uses Y-down for the SVG/PDF output coordinate
   * system, so the line box's top edge is at -ascender. */
  float total_x = 0.f;
  for (unsigned i = 0; i < len; i++) total_x += pos[i].x_advance;
  hb_font_extents_t fe;
  hb_font_get_h_extents (font, &fe);
  float asc = (float) fe.ascender;   /* positive */
  float desc = (float) fe.descender; /* negative */
  /* Advances and h_extents are in input space (pixel*SCALE);
   * set_extents divides by the context's scale_factor, so we
   * pass them through without pre-scaling. */
  hb_vector_extents_t logical = { 0.f, -asc, total_x, asc - desc };
  if (p) hb_vector_paint_set_extents (p, &logical);
  else   hb_vector_draw_set_extents  (d, &logical);

  float pen_x = 0.f;
  float pen_y = 0.f;
  for (unsigned i = 0; i < len; i++)
  {
    if (p)
      hb_vector_paint_glyph (p, font, info[i].codepoint,
                             pen_x + pos[i].x_offset,
                             pen_y + pos[i].y_offset,
                             HB_VECTOR_EXTENTS_MODE_EXPAND);
    else
      hb_vector_draw_glyph (d, font, info[i].codepoint,
                            pen_x + pos[i].x_offset,
                            pen_y + pos[i].y_offset,
                            HB_VECTOR_EXTENTS_MODE_EXPAND);
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
  hb_face_t *face = hb_face_create (blob, 0);
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
      hb_raster_paint_glyph (p, font, info[i].codepoint, gx, gy);
      img = hb_raster_paint_render (p);
    }
    else
    {
      hb_raster_draw_reset (d);
      hb_raster_draw_set_extents (d, &ext);
      hb_raster_draw_set_scale_factor (d, (float) SCALE, (float) SCALE);
      hb_raster_draw_glyph (d, font, info[i].codepoint, gx, gy);
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
