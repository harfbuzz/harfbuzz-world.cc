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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, nullptr, 0);

  *out_face = face;
  *out_font = font;
  return buf;
}

/* Returns a malloc'd JSON string of the shaped glyph stream:
 *   [{"gid":N,"cluster":N,"x_offset":N,"y_offset":N,"x_advance":N,"y_advance":N},...]
 * Caller frees with web_free_string(). */
EMSCRIPTEN_KEEPALIVE
char *web_shape_json (const uint8_t *font_bytes, unsigned font_len,
                      const char *utf8_text)
{
  hb_face_t *face = nullptr;
  hb_font_t *font = nullptr;
  hb_buffer_t *buf = shape (font_bytes, font_len, utf8_text, &face, &font);
  if (!buf)
    return strdup ("[]");

  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, nullptr);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, nullptr);

  /* Estimate JSON capacity:  ~96 bytes per glyph entry. */
  size_t cap = 16 + 96 * (size_t) len + 1;
  char *out = (char *) malloc (cap);
  size_t off = 0;
  off += snprintf (out + off, cap - off, "[");
  for (unsigned i = 0; i < len; i++)
  {
    off += snprintf (out + off, cap - off,
                     "%s{\"gid\":%u,\"cluster\":%u,"
                     "\"x_offset\":%d,\"y_offset\":%d,"
                     "\"x_advance\":%d,\"y_advance\":%d}",
                     i ? "," : "",
                     info[i].codepoint, info[i].cluster,
                     pos[i].x_offset, pos[i].y_offset,
                     pos[i].x_advance, pos[i].y_advance);
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

  /* Render at @font_size_px pixels per em: shaped positions then
   * come back in pixel space, and the SVG carries pixel coords. */
  hb_font_set_scale (font, (int) font_size_px, (int) font_size_px);
  /* Re-shape with the new scale so positions are in pixels. */
  hb_buffer_clear_contents (buf);
  hb_buffer_add_utf8 (buf, utf8_text, -1, 0, -1);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (font, buf, nullptr, 0);

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
    p = hb_vector_paint_create_or_fail (format);
  else
    d = hb_vector_draw_create_or_fail (format);

  /* Namespace SVG ids per render so multiple hb-vector
   * SVGs embedded in the same page (shape tab vs vector
   * tab) can't collide on short IDs like "c0" / "gr0". */
  if (format == HB_VECTOR_FORMAT_SVG)
  {
    static unsigned s_counter = 0;
    char pfx[16];
    snprintf (pfx, sizeof pfx, "v%u-", ++s_counter);
    if (p) hb_vector_paint_svg_set_prefix (p, pfx);
    if (d) hb_vector_draw_svg_set_prefix  (d, pfx);
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

  /* Shape happened at the font's default upem scale; positions
   * are in font units.  Convert to pixels via a scale factor on
   * the raster context. */
  unsigned upem = hb_face_get_upem (face);
  float scale  = font_size_px / (float) upem;

  /* Total advance and font metrics in font units. */
  unsigned len = hb_buffer_get_length (buf);
  hb_glyph_info_t    *info = hb_buffer_get_glyph_infos (buf, nullptr);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, nullptr);
  float total_x = 0.f;
  for (unsigned i = 0; i < len; i++) total_x += pos[i].x_advance;
  hb_font_extents_t fe;
  hb_font_get_h_extents (font, &fe);
  float ascent  = (float) fe.ascender;
  float descent = (float) -fe.descender;

  /* Pixel-space buffer dimensions. */
  unsigned w = (unsigned) (total_x * scale + 0.999f);
  unsigned h = (unsigned) ((ascent + descent) * scale + 0.999f);
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
  if (is_color) p = hb_raster_paint_create_or_fail ();
  else          d = hb_raster_draw_create_or_fail ();

  /* Extents live in pixel space (Y-up): bottom edge at
   * -descent_px places the descender row at row 0 of the buffer
   * and the ascender row at the top.  pen_y stays at 0
   * (baseline) and gets scaled by set_scale_factor below. */
  unsigned stride = w * 4;
  int descent_px = (int) (descent * scale);
  hb_raster_extents_t ext = { 0, -descent_px, w, h, stride };

  uint8_t *out = (p || d) ? (uint8_t *) calloc ((size_t) stride * h, 1)
                          : nullptr;
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
      hb_raster_paint_set_scale_factor (p, 1.f / scale, 1.f / scale);
      hb_raster_paint_glyph (p, font, info[i].codepoint, gx, gy);
      img = hb_raster_paint_render (p);
    }
    else
    {
      hb_raster_draw_reset (d);
      hb_raster_draw_set_extents (d, &ext);
      hb_raster_draw_set_scale_factor (d, 1.f / scale, 1.f / scale);
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
          if (cov == 255) { dpx = 0xFF000000u; }
          else
          {
            unsigned inv = 255 - cov;
            uint8_t rb = (uint8_t) (((dpx & 0xFF) * inv + 127) / 255);
            uint8_t rg = (uint8_t) ((((dpx >> 8) & 0xFF) * inv + 127) / 255);
            uint8_t rr = (uint8_t) ((((dpx >> 16) & 0xFF) * inv + 127) / 255);
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
