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

} /* extern "C" */
