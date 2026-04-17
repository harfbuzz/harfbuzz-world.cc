/* HarfBuzz World — single-page demo shell.
 *
 * Loads wasm + the default font once, then routes the shared
 * text/size controls to whichever demo is currently active.
 * Active demo is driven by the URL hash (#shape, #vector,
 * #raster) so links and back/forward navigation work. */

/* IndexedDB font cache — persists uploaded font bytes across
 * page reloads, keyed by a short SHA-256 hash prefix. */
const FONT_DB = "hb-font-cache";
const FONT_STORE = "fonts";
function fontDbOpen () {
  return new Promise ((resolve, reject) => {
    const req = indexedDB.open (FONT_DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore (FONT_STORE);
    req.onsuccess = () => resolve (req.result);
    req.onerror = () => reject (req.error);
  });
}
function fontDbPut (db, key, value) {
  return new Promise ((resolve, reject) => {
    const tx = db.transaction (FONT_STORE, "readwrite");
    const store = tx.objectStore (FONT_STORE);
    store.clear ();
    store.put (value, key);
    tx.oncomplete = () => resolve ();
    tx.onerror = () => reject (tx.error);
  });
}
function fontDbGet (db, key) {
  return new Promise ((resolve, reject) => {
    const tx = db.transaction (FONT_STORE, "readonly");
    const req = tx.objectStore (FONT_STORE).get (key);
    req.onsuccess = () => resolve (req.result);
    req.onerror = () => reject (req.error);
  });
}
async function fontHash (bytes) {
  const digest = await crypto.subtle.digest ("SHA-256", bytes);
  const arr = Array.from (new Uint8Array (digest));
  return arr.slice (0, 8).map (b => b.toString (16).padStart (2, "0")).join ("");
}

(async function main () {
  const Module = await createHbWorld ();

  /* Single owner of the wasm-side font blob.  Swapped in place
   * when the user picks a different font; the old buffer is
   * freed before the new one is allocated. */
  let fontBuf = null;
  let fontPtr = 0;
  function setFontBytes (bytes, displayName) {
    if (fontPtr) Module._free (fontPtr);
    fontBuf = bytes;
    fontPtr = Module._malloc (fontBuf.length);
    Module.HEAPU8.set (fontBuf, fontPtr);
    /* Prefer the font's own family name (hb-ot-name) over
     * whatever caller-friendly label was passed in. */
    const namePtr = Module._web_font_family (fontPtr, fontBuf.length);
    const otName = Module.UTF8ToString (namePtr);
    Module._web_free_string (namePtr);
    fontNameEl.textContent = otName || displayName || "";
    /* Push the font to the GPU iframe FIRST, so the subsequent
     * refresh*() calls (which postGpu variations + palette)
     * land on the new font.  Otherwise web_load_font would
     * recreate the demo_font with default state and silently
     * drop our previously-pushed configuration. */
    if (gpuReady)
      postGpu ({ kind: "font", bytes: fontBuf.buffer.slice (0) });
    refreshAxes ();
    refreshPalettes ();
    renderActive ();
  }

  const textInput     = document.getElementById ("text");
  const sizeInput     = document.getElementById ("size");
  const fontButton    = document.getElementById ("font-button");
  const fontMenu      = document.getElementById ("font-menu");
  const fontShipped   = document.getElementById ("font-shipped");
  const fontInput     = document.getElementById ("font-input");
  const fontUrl       = document.getElementById ("font-url");
  const fontUrlLoad   = document.getElementById ("font-url-load");
  const fontGf        = document.getElementById ("font-gf");
  const fontGfList    = document.getElementById ("font-gf-list");
  const fontGfLoad    = document.getElementById ("font-gf-load");
  const fontNameEl    = document.getElementById ("font-name");
  const dropOverlay   = document.getElementById ("drop-overlay");
  const paletteLabel  = document.getElementById ("palette-label");
  const paletteSelect = document.getElementById ("palette");

  /* Helper: marshal current text into wasm memory.  Caller frees. */
  function withText (fn) {
    const text = textInput.value;
    const textLen = Module.lengthBytesUTF8 (text) + 1;
    const textPtr = Module._malloc (textLen);
    Module.stringToUTF8 (text, textPtr, textLen);
    try { return fn (textPtr); }
    finally { Module._free (textPtr); }
  }

  function currentSize () {
    return parseFloat (sizeInput.value) || 72;
  }

  /* Demos.  Each exposes a render() that reads the shared
   * controls and updates its DOM. */

  const shapeRender  = document.getElementById ("shape-render");
  const shapeGlyphs    = document.getElementById ("shape-glyphs");
  const shapeShowNames = document.getElementById ("shape-show-names");
  const shapeClusterLvl = document.getElementById ("shape-cluster-level");
  shapeClusterLvl.addEventListener ("change", () => {
    Module._web_set_cluster_level (parseInt (shapeClusterLvl.value, 10) || 0);
    renderActive ();
  });
  let lastShapeGlyphs  = [];
  /* Render the shaped-glyph stream as a table so columns line
   * up and a faint divider visually groups consecutive glyphs
   * that share a cluster (= came from the same input
   * codepoint).  First column toggles between gid (numeric)
   * and glyph name -- gid is what most APIs return; names are
   * what humans recognize while authoring fonts. */
  function escapeHtml (s) {
    return String (s).replace (/[&<>]/g, (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
  }
  function renderGlyphTable (glyphs) {
    lastShapeGlyphs = glyphs;
    if (!glyphs.length) { shapeGlyphs.innerHTML = ""; return; }
    const useName = shapeShowNames.checked;
    const idCol = useName ? "name" : "gid";
    const idHeader = useName ? "glyph name" : "glyph index";
    const cols = [idCol, "cluster", "x_advance", "y_advance", "x_offset", "y_offset"];
    const headers = [idHeader, "cluster", "x_advance", "y_advance", "x_offset", "y_offset"];
    let html = "<table class=\"glyph-table\"><thead><tr>";
    for (const h of headers) html += "<th>" + h + "</th>";
    html += "</tr></thead><tbody>";
    let prevCluster = glyphs[0].cluster;
    for (const g of glyphs) {
      const cls = g.cluster !== prevCluster ? " class=\"cluster-break\"" : "";
      html += "<tr" + cls + ">";
      for (const c of cols) html += "<td>" + escapeHtml (g[c]) + "</td>";
      html += "</tr>";
      prevCluster = g.cluster;
    }
    html += "</tbody></table>";
    shapeGlyphs.innerHTML = html;
  }
  shapeShowNames.addEventListener ("change", () => renderGlyphTable (lastShapeGlyphs));
  function renderShape () {
    withText ((textPtr) => {
      const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                              textPtr, currentSize ());
      shapeRender.innerHTML = Module.UTF8ToString (svgPtr);
      Module._web_free_string (svgPtr);

      const jsonPtr = Module._web_shape_json (fontPtr, fontBuf.length, textPtr);
      const glyphs = JSON.parse (Module.UTF8ToString (jsonPtr));
      Module._web_free_string (jsonPtr);
      renderGlyphTable (glyphs);
    });
    renderSnippet ("shape");
  }

  /* ---- Code snippets ---- */
  const SNIPPETS = {
    shape: {
      headline: "hb_shape",
      template:
`hb_blob_t *blob = hb_blob_create_from_file ("{font}");
hb_face_t *face = hb_face_create (blob, 0);
hb_font_t *font = hb_font_create (face);

hb_buffer_t *buf = hb_buffer_create ();
hb_buffer_add_utf8 (buf, "{text}", -1, 0, -1);
hb_buffer_guess_segment_properties (buf);  /* toy: real apps set script/lang/dir explicitly */

hb_shape (font, buf, NULL, 0);

unsigned len = hb_buffer_get_length (buf);
hb_glyph_info_t     *info = hb_buffer_get_glyph_infos     (buf, NULL);
hb_glyph_position_t *pos  = hb_buffer_get_glyph_positions (buf, NULL);

/* ... use info[i].codepoint, pos[i].x_advance, etc. ... */

hb_buffer_destroy (buf);
hb_font_destroy (font);
hb_face_destroy (face);
hb_blob_destroy (blob);`
    },
    subset: {
      headline: "hb_subset_or_fail",
      template:
`hb_blob_t *blob = hb_blob_create_from_file ("{font}");
hb_face_t *face = hb_face_create (blob, 0);

hb_subset_input_t *input = hb_subset_input_create_or_fail ();
hb_set_t *unicodes = hb_subset_input_unicode_set (input);
const char *text = "{text}";
for (const char *p = text; *p; ) {
  /* decode next UTF-8 codepoint into 'cp' */
  hb_codepoint_t cp = /* ... */ 0;
  hb_set_add (unicodes, cp);
  /* advance p */
}

hb_face_t *subset = hb_subset_or_fail (face, input);
hb_blob_t *out = hb_face_reference_blob (subset);
/* ...write hb_blob_get_data(out, NULL) to disk... */

hb_blob_destroy (out);
hb_face_destroy (subset);
hb_subset_input_destroy (input);
hb_face_destroy (face);
hb_blob_destroy (blob);`
    },
    raster: {
      headline: ["hb_raster_paint_render", "hb_raster_draw_render"],
      template:
`#define FONT_SIZE_PX  {size}
#define SUBPIXEL_BITS 6              /* 26.6 fixed-point, like FreeType */
#define SCALE         (1 << SUBPIXEL_BITS)

hb_blob_t *blob = hb_blob_create_from_file ("{font}");
hb_face_t *face = hb_face_create (blob, 0);
hb_font_t *font = hb_font_create (face);
/* Shape positions in pixel*SCALE units for sub-pixel
 * precision; the raster context divides input coords by
 * SCALE to land on pixels at render time. */
hb_font_set_scale (font, FONT_SIZE_PX * SCALE, FONT_SIZE_PX * SCALE);

hb_buffer_t *buf = hb_buffer_create ();
hb_buffer_add_utf8 (buf, "{text}", -1, 0, -1);
hb_buffer_guess_segment_properties (buf);  /* toy: real apps set script/lang/dir explicitly */
hb_shape (font, buf, NULL, 0);

hb_raster_extents_t ext = { /*x*/ 0, /*y*/ 0, /*w*/ 0, /*h*/ 0, /*stride*/ 0 };
/* ... compute ext in pixels from buffer's advances + font h_extents ... */

/* Color fonts go through hb_raster_paint_*; mono outline
 * fonts can use the cheaper hb_raster_draw_* path. */
hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                     hb_ot_color_has_layers (face) ||
                     hb_ot_color_has_png (face);
hb_raster_paint_t *p = is_color ? hb_raster_paint_create_or_fail () : NULL;
hb_raster_draw_t  *d = is_color ? NULL : hb_raster_draw_create_or_fail ();

unsigned len = hb_buffer_get_length (buf);
hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, NULL);
hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, NULL);

float pen_x = 0, pen_y = 0;
for (unsigned i = 0; i < len; i++) {
  float gx = pen_x + pos[i].x_offset;
  float gy = pen_y + pos[i].y_offset;
  hb_raster_image_t *img;
  if (p) {
    hb_raster_paint_set_extents (p, &ext);
    hb_raster_paint_set_scale_factor (p, SCALE, SCALE);
    hb_raster_paint_glyph (p, font, info[i].codepoint, gx, gy);
    img = hb_raster_paint_render (p);  /* BGRA32 premultiplied */
  } else {
    hb_raster_draw_reset (d);
    hb_raster_draw_set_extents (d, &ext);
    hb_raster_draw_set_scale_factor (d, SCALE, SCALE);
    hb_raster_draw_glyph (d, font, info[i].codepoint, gx, gy);
    img = hb_raster_draw_render (d);  /* A8 coverage */
  }
  /* ...SRC_OVER composite img onto your output... */
  pen_x += pos[i].x_advance;
  pen_y += pos[i].y_advance;
}

hb_raster_paint_destroy (p);
hb_raster_draw_destroy (d);
hb_buffer_destroy (buf);
hb_font_destroy (font);
hb_face_destroy (face);
hb_blob_destroy (blob);`
    },
    vector: {
      headline: ["hb_vector_paint_render", "hb_vector_draw_render"],
      template:
`#define FONT_SIZE_PX  {size}
#define SUBPIXEL_BITS 6              /* 26.6 fixed-point, like FreeType */
#define SCALE         (1 << SUBPIXEL_BITS)

hb_blob_t *blob = hb_blob_create_from_file ("{font}");
hb_face_t *face = hb_face_create (blob, 0);
hb_font_t *font = hb_font_create (face);
hb_font_set_scale (font, FONT_SIZE_PX * SCALE, FONT_SIZE_PX * SCALE);

hb_buffer_t *buf = hb_buffer_create ();
hb_buffer_add_utf8 (buf, "{text}", -1, 0, -1);
hb_buffer_guess_segment_properties (buf);  /* toy: real apps set script/lang/dir explicitly */
hb_shape (font, buf, NULL, 0);

/* Color fonts go through hb_vector_paint_*; mono outline
 * fonts can use the cheaper hb_vector_draw_* path. */
hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                     hb_ot_color_has_layers (face) ||
                     hb_ot_color_has_png (face);
hb_vector_paint_t *p = is_color
  ? hb_vector_paint_create_or_fail (HB_VECTOR_FORMAT_SVG) : NULL;
hb_vector_draw_t  *d = is_color
  ? NULL : hb_vector_draw_create_or_fail (HB_VECTOR_FORMAT_SVG);
/* Tell vector how many input units fit in one output pixel
 * (matches what hb-vector / hb-raster utils do). */
if (p) hb_vector_paint_set_scale_factor (p, SCALE, SCALE);
else   hb_vector_draw_set_scale_factor  (d, SCALE, SCALE);

unsigned len = hb_buffer_get_length (buf);
hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, NULL);
hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, NULL);

float pen_x = 0, pen_y = 0;
for (unsigned i = 0; i < len; i++) {
  float gx = pen_x + pos[i].x_offset;
  float gy = pen_y + pos[i].y_offset;
  if (p)
    hb_vector_paint_glyph (p, font, info[i].codepoint, gx, gy,
                           HB_VECTOR_EXTENTS_MODE_EXPAND);
  else
    hb_vector_draw_glyph  (d, font, info[i].codepoint, gx, gy,
                           HB_VECTOR_EXTENTS_MODE_EXPAND);
  pen_x += pos[i].x_advance;
  pen_y += pos[i].y_advance;
}

hb_blob_t *out = p ? hb_vector_paint_render (p)
                   : hb_vector_draw_render  (d);
/* ...write hb_blob_get_data(out, NULL) somewhere... */

hb_blob_destroy (out);
hb_vector_paint_destroy (p);
hb_vector_draw_destroy (d);
hb_buffer_destroy (buf);
hb_font_destroy (font);
hb_face_destroy (face);
hb_blob_destroy (blob);`
    },
    gpu: {
      headline: ["hb_gpu_paint_encode", "hb_gpu_draw_encode"],
      template:
`hb_blob_t *blob = hb_blob_create_from_file ("{font}");
hb_face_t *face = hb_face_create (blob, 0);
hb_font_t *font = hb_font_create (face);

/* Color fonts (COLR layers / paint tree) go through
 * hb_gpu_paint_*; mono outline fonts through the cheaper
 * hb_gpu_draw_* path.  Bitmap color glyphs (CBDT/sbix/PNG)
 * are NOT supported by hb-gpu -- shapes/paths only. */
hb_bool_t is_color = hb_ot_color_has_paint (face) ||
                     hb_ot_color_has_layers (face);
hb_gpu_paint_t *p = is_color ? hb_gpu_paint_create_or_fail () : NULL;
hb_gpu_draw_t  *d = is_color ? NULL : hb_gpu_draw_create_or_fail ();

/* HB_GPU_SHADER_LANG_GLSL / _WGSL / _MSL / _HLSL all available --
 * pick the one your backend (OpenGL, WebGPU, Metal, D3D12) needs. */
hb_gpu_shader_lang_t lang = HB_GPU_SHADER_LANG_GLSL;
const char *frag = is_color
  ? hb_gpu_paint_shader_source (HB_GPU_SHADER_STAGE_FRAGMENT, lang)
  : hb_gpu_draw_shader_source  (HB_GPU_SHADER_STAGE_FRAGMENT, lang);
const char *vert = hb_gpu_shader_source (HB_GPU_SHADER_STAGE_VERTEX, lang);
/* ...compile {vert,frag} into your renderer's pipeline... */

/* Per-glyph: encode the outline / paint tree into a compact
 * blob that the GPU shader decodes + rasterizes.  Cache the
 * (glyph_id -> atlas_offset) mapping in your renderer so
 * each glyph is encoded + uploaded at most once per font;
 * subsequent draws of the same glyph just emit a quad with
 * the cached atlas offset. */
unsigned len = hb_buffer_get_length (buf);  /* assume buf is shaped */
hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, NULL);
hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, NULL);
for (unsigned i = 0; i < len; i++) {
  unsigned gid = info[i].codepoint;
  /* if (cache_lookup (gid)) { emit_quad (...); continue; } */
  hb_glyph_extents_t ext;
  hb_blob_t *enc;
  if (p) {
    hb_gpu_paint_glyph (p, font, gid);
    enc = hb_gpu_paint_encode (p, &ext);
  } else {
    hb_gpu_draw_glyph (d, font, gid);
    enc = hb_gpu_draw_encode (d, &ext);
  }
  /* ...upload enc bytes to atlas, store offset in cache,
   *    emit a quad at the glyph's pen position with the
   *    atlas offset as a vertex attribute... */
  hb_blob_destroy (enc);
}

hb_gpu_paint_destroy (p);
hb_gpu_draw_destroy (d);
hb_font_destroy (font);
hb_face_destroy (face);
hb_blob_destroy (blob);`
    },
  };
  /* Look up an hb_* identifier's docs section in the
   * generated HB_DOC_SYMBOLS table (sourced from the
   * authoritative harfbuzz-sections.txt).  Returns null if
   * unknown so the linkifier can leave the token unlinked
   * rather than emit a broken link. */
  function hbDocsUrl (name) {
    const map = window.HB_DOC_SYMBOLS;
    const section = map ? map[name] : null;
    if (!section) return null;
    return "https://harfbuzz.github.io/harfbuzz-hb-" + section + ".html#"
         + name.replace (/_/g, "-");
  }
  function escapeForC (s) {
    return s.replace (/\\/g, "\\\\")
            .replace (/"/g, "\\\"")
            .replace (/\n/g, "\\n");
  }
  /* Walk over the highlighted HTML and turn known hb_*
   * identifiers into anchors pointing at the official docs.
   * Unknown identifiers (or pseudocode placeholders) are
   * left as plain text. */
  function linkifyHbCalls (html, headline) {
    const headlines = Array.isArray (headline) ? new Set (headline)
                                               : new Set ([headline]);
    return html.replace (/(hb_[a-z0-9_]+|HB_[A-Z0-9_]+)/g, (m) => {
      const url = hbDocsUrl (m);
      const isHeadline = headlines.has (m);
      if (!url)
        return isHeadline ? "<strong>" + m + "</strong>" : m;
      const wrapped = "<a href=\"" + url + "\" target=\"_blank\" rel=\"noopener\">" + m + "</a>";
      return isHeadline ? "<strong>" + wrapped + "</strong>" : wrapped;
    });
  }
  function renderSnippet (key) {
    const def = SNIPPETS[key];
    if (!def) return;
    const el = document.getElementById (key + "-snippet");
    if (!el) return;
    const fontPath = (function () {
      const u = new URL (location.href);
      const f = u.searchParams.get ("font");
      if (f) return f.split ("/").pop ();
      /* Preset / shipped picks have the preset key in the URL;
       * resolve via PRESETS for a clean filename. */
      const presetKey = u.searchParams.get ("preset");
      if (presetKey && PRESETS[presetKey])
        return PRESETS[presetKey].font.split ("/").pop ();
      /* Last resort: build something printable from the OT
       * family name. */
      return (fontNameEl.textContent || "font").replace (/\s+/g, "") + ".ttf";
    }) ();
    /* Wrap substituted text fields in U+2068 FIRST STRONG
     * ISOLATE / U+2069 POP DIRECTIONAL ISOLATE so embedded
     * RTL strings (Hebrew, Arabic) don't bidi-reorder against
     * the surrounding LTR C code (e.g. "..." -1, 0, -1) in
     * the rendered HTML.  Invisible to text/clipboard. */
    const isolate = (s) => "\u2068" + s + "\u2069";
    const code = def.template
      .replaceAll ("{font}", isolate (escapeForC (fontPath)))
      .replaceAll ("{text}", isolate (escapeForC (textInput.value)))
      .replaceAll ("{size}", String (currentSize ()));
    /* hljs may not be loaded yet on first render; in that
     * case just show the raw code, then re-highlight when
     * highlight.js arrives. */
    el.textContent = code;
    if (window.hljs) {
      const result = hljs.highlight (code, { language: "c" });
      el.innerHTML = linkifyHbCalls (result.value, def.headline);
    }
  }
  /* Re-render snippets once highlight.js finishes loading,
   * and highlight any static <code class="language-*"> blocks
   * on the embed/welcome tab. */
  window.addEventListener ("load", () => {
    for (const key of Object.keys (SNIPPETS)) renderSnippet (key);
    if (window.hljs) {
      document.querySelectorAll ("pre.code code[class*=\"language-\"]")
        .forEach ((el) => {
          hljs.highlightElement (el);
          /* Linkify hb_* / HB_* identifiers to their docs,
           * same as the live snippet path does. */
          el.innerHTML = linkifyHbCalls (el.innerHTML, null);
        });
    }
  });

  /* Theme toggle: flips between light and dark.  For a
   * first-time visitor (no data-theme set), the OS pref
   * controls the initial render via CSS @media; the button
   * icon reflects that effective theme so the first click
   * predictably goes to the opposite. */
  const themeToggle = document.getElementById ("theme-toggle");
  function effectiveTheme () {
    const pinned = document.documentElement.dataset.theme;
    if (pinned === "light" || pinned === "dark") return pinned;
    return matchMedia ("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  function applyTheme (t) {
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem ("theme", t); } catch {}
    themeToggle.textContent = t === "dark" ? "☾" : "☀";
  }
  themeToggle.textContent = effectiveTheme () === "dark" ? "☾" : "☀";
  themeToggle.addEventListener ("click", () => {
    applyTheme (effectiveTheme () === "dark" ? "light" : "dark");
  });

  /* Code snippet <details> (those with data-snippet) share
   * open/closed state so toggling one opens/closes all. */
  const codeSnippetEls = () => document.querySelectorAll ("details[data-snippet]");
  function applySnippetOpen (open) {
    codeSnippetEls ().forEach ((d) => { d.open = open; });
  }
  /* Sync code snippets open/close together. */
  codeSnippetEls ().forEach ((d) => {
    d.addEventListener ("toggle", () => {
      const open = d.open;
      codeSnippetEls ().forEach ((other) => {
        if (other !== d && other.open !== open) other.open = open;
      });
    });
  });
  /* Wire up buttons on all .snippet details (code + tables). */
  function flash (btn, msg) {
    const old = btn.textContent;
    btn.textContent = msg;
    setTimeout (() => { btn.textContent = old; }, 1200);
  }
  document.querySelectorAll ("details.snippet").forEach ((d) => {
    const linkBtn = d.querySelector (".snippet-link");
    const copyBtn = d.querySelector (".snippet-copy");
    if (linkBtn) {
      const sub = d.dataset.snippet ? "code"
                : d.dataset.section ? d.dataset.section
                : "tables";
      function linkUrl () {
        const tab = d.dataset.snippet || activeName || "embed";
        const u = new URL (location.href);
        u.hash = tab + "/" + sub;
        return u.toString ();
      }
      linkBtn.addEventListener ("mouseenter", () => {
        linkBtn.title = linkUrl ();
      });
      linkBtn.addEventListener ("click", (e) => {
        e.preventDefault ();
        e.stopPropagation ();
        navigator.clipboard.writeText (linkUrl ()).then (
          () => flash (linkBtn, "✓"),
          () => flash (linkBtn, "✗"));
      });
    }
    if (copyBtn)
      copyBtn.addEventListener ("click", (e) => {
        e.preventDefault ();
        e.stopPropagation ();
        const code = d.querySelector ("pre code").innerText;
        navigator.clipboard.writeText (code).then (
          () => flash (copyBtn, "✓"),
          () => flash (copyBtn, "✗"));
      });
  });

  const vectorRender = document.getElementById ("vector-render");
  const vectorStats  = document.getElementById ("vector-stats");
  const dlSvg        = document.getElementById ("vector-dl-svg");
  const dlPdf        = document.getElementById ("vector-dl-pdf");
  const svgSizeEl    = document.getElementById ("vector-svg-size");
  const pdfSizeEl    = document.getElementById ("vector-pdf-size");
  let svgUrl = null, pdfUrl = null;
  function renderVector () {
    withText ((textPtr) => {
      const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                              textPtr, currentSize ());
      const svg = Module.UTF8ToString (svgPtr);
      Module._web_free_string (svgPtr);
      vectorRender.innerHTML = svg;
      const svgEl = vectorRender.querySelector ("svg");
      if (svgEl) {
        const w = svgEl.getAttribute ("width");
        const h = svgEl.getAttribute ("height");
        const vb = svgEl.getAttribute ("viewBox");
        vectorStats.textContent = w + " × " + h + " px (viewBox: " + vb + ")";
      } else {
        vectorStats.textContent = "";
      }
      if (svgUrl) URL.revokeObjectURL (svgUrl);
      const svgBlob = new Blob ([svg], { type: "image/svg+xml" });
      svgUrl = URL.createObjectURL (svgBlob);
      dlSvg.href = svgUrl;
      svgSizeEl.textContent = fmtBytes (svgBlob.size);

      const lenPtr = Module._malloc (4);
      const pdfPtr = Module._web_render_pdf (fontPtr, fontBuf.length,
                                              textPtr, currentSize (), lenPtr);
      const pdfLen = new Uint32Array (Module.HEAPU8.buffer, lenPtr, 1)[0];
      const pdfBytes = Module.HEAPU8.slice (pdfPtr, pdfPtr + pdfLen);
      Module._web_free_string (pdfPtr);
      Module._free (lenPtr);
      if (pdfUrl) URL.revokeObjectURL (pdfUrl);
      pdfUrl = URL.createObjectURL (new Blob ([pdfBytes], { type: "application/pdf" }));
      dlPdf.href = pdfUrl;
      pdfSizeEl.textContent = fmtBytes (pdfLen);
    });
    renderSnippet ("vector");
  }

  const subsetOrig    = document.getElementById ("subset-orig-size");
  const subsetNew     = document.getElementById ("subset-new-size");
  const subsetSaving  = document.getElementById ("subset-saving");
  const subsetDl      = document.getElementById ("subset-download");
  const subsetDlSize  = document.getElementById ("subset-dl-size");
  const subsetPreview = document.getElementById ("subset-preview");
  const subsetBarFill = document.getElementById ("subset-bar-fill");
  const subsetInstantiate = document.getElementById ("subset-instantiate");
  const subsetInstantiateLabel = document.getElementById ("subset-instantiate-label");
  subsetInstantiate.addEventListener ("change", () => {
    Module._web_set_subset_instantiate (subsetInstantiate.checked ? 1 : 0);
    renderActive ();
  });
  const subsetHint    = document.getElementById ("subset-hint");
  const subsetCounts  = document.getElementById ("subset-counts");
  const subsetTablesWrap = document.getElementById ("subset-tables-wrap");
  const subsetTables  = document.getElementById ("subset-tables");
  function fontStats (ptr, len) {
    const p = Module._web_font_stats (ptr, len);
    const s = JSON.parse (Module.UTF8ToString (p));
    Module._web_free_string (p);
    return s;
  }
  function renderSubsetTables (orig, sub) {
    const subBy = new Map (sub.tables.map ((t) => [t.tag, t.size]));
    const allTags = new Set ([...orig.tables.map ((t) => t.tag), ...subBy.keys ()]);
    const rows = [];
    const origBy = new Map (orig.tables.map ((t) => [t.tag, t.size]));
    for (const tag of allTags) {
      const o = origBy.get (tag) || 0;
      const n = subBy.get (tag) || 0;
      rows.push ({ tag, before: o, after: n, delta: n - o });
    }
    rows.sort ((a, b) => (b.before - b.after) - (a.before - a.after));
    let html = "<table class=\"glyph-table subset-table\"><thead><tr>"
             + "<th>tag</th><th>before</th><th>after</th><th>saved</th>"
             + "</tr></thead><tbody>";
    for (const r of rows) {
      const saved = r.before - r.after;
      const cls = saved > 0 ? "" : (r.after > r.before ? " class=\"subset-grew\"" : " class=\"subset-same\"");
      html += "<tr" + cls + "><td>" + r.tag
            + "</td><td>" + fmtBytes (r.before)
            + "</td><td>" + fmtBytes (r.after)
            + "</td><td>" + (saved > 0 ? fmtBytes (saved) : "—")
            + "</td></tr>";
    }
    html += "</tbody></table>";
    subsetTables.innerHTML = html;
  }
  /* Dynamic @font-face rule; bytes change on every render. */
  const subsetStyle = document.createElement ("style");
  document.head.appendChild (subsetStyle);
  let subsetGen = 0;
  let subsetUrl = null;
  function bytesToBase64 (bytes) {
    /* Chunked to avoid "Maximum call stack size exceeded"
     * from spread on multi-hundred-KB arrays. */
    let s = "";
    for (let i = 0; i < bytes.length; i += 0x8000)
      s += String.fromCharCode.apply (null, bytes.subarray (i, i + 0x8000));
    return btoa (s);
  }
  function fmtBytes (n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed (1) + " KB";
    return (n / (1024 * 1024)).toFixed (2) + " MB";
  }
  function renderSubset () {
    subsetOrig.textContent = fmtBytes (fontBuf.length);
    withText ((textPtr) => {
      const lenPtr = Module._malloc (4);
      const dataPtr = Module._web_subset (fontPtr, fontBuf.length,
                                           textPtr, lenPtr);
      if (!dataPtr) {
        Module._free (lenPtr);
        subsetNew.textContent    = "(failed)";
        subsetSaving.textContent = "—";
        subsetDl.removeAttribute ("href");
        return;
      }
      const sublen = new Uint32Array (Module.HEAPU8.buffer, lenPtr, 1)[0];
      const bytes  = Module.HEAPU8.slice (dataPtr, dataPtr + sublen);
      Module._web_free_string (dataPtr);
      Module._free (lenPtr);

      subsetOrig.textContent = fmtBytes (fontBuf.length);
      subsetNew.textContent = fmtBytes (sublen);
      const savedBytes = fontBuf.length - sublen;
      const savedPct = (100 * savedBytes / fontBuf.length).toFixed (1);
      subsetSaving.textContent = fmtBytes (savedBytes) + " (" + savedPct + "%)";
      subsetBarFill.style.width = (100 * sublen / fontBuf.length).toFixed (2) + "%";
      subsetDlSize.textContent = fmtBytes (sublen);

      /* Per-table breakdown + glyph/Unicode counts.  Allocate a
       * temp wasm buffer for the subset bytes since the existing
       * fontPtr only holds the original font. */
      const origStats = fontStats (fontPtr, fontBuf.length);
      const subPtr = Module._malloc (sublen);
      Module.HEAPU8.set (bytes, subPtr);
      const subStats = fontStats (subPtr, sublen);
      Module._free (subPtr);
      subsetCounts.textContent =
        "Kept " + subStats.num_glyphs + " of " + origStats.num_glyphs + " glyphs"
        + " · " + subStats.num_unicodes + " of " + origStats.num_unicodes + " Unicode codepoints";
      renderSubsetTables (origStats, subStats);
      renderSnippet ("subset");
      subsetTablesWrap.hidden = false;

      /* If the loaded font is itself already small (i.e. mostly
       * meta + tiny glyf), the savings will be modest no matter
       * what.  Surface that so users don't think the subsetter
       * is broken. */
      if (savedPct < 20 && origStats.num_glyphs < 5000) {
        subsetHint.textContent = "Low savings: this font is already tight"
          + " (" + origStats.num_glyphs + " glyphs)."
          + " Try a large family to see the subsetter shine.";
        subsetHint.hidden = false;
      } else {
        subsetHint.hidden = true;
      }

      if (subsetUrl) URL.revokeObjectURL (subsetUrl);
      subsetUrl = URL.createObjectURL (new Blob ([bytes], { type: "font/ttf" }));
      subsetDl.href = subsetUrl;

      /* Preview: render the current text using the subset
       * font itself, via a dynamic @font-face whose src is a
       * fresh base64 data URL every time.  A unique family
       * name per render forces the browser to adopt the new
       * bytes instead of holding on to a cached face. */
      const family = "hbSubset" + (++subsetGen);
      const b64 = bytesToBase64 (bytes);
      /* Mirror the picker's palette in the native font render
       * via @font-palette-values + font-palette.  The named
       * palette stays --hbPalette so the style block can be
       * regenerated freely without rebinding the preview. */
      const palIdx = parseInt (paletteSelect.value, 10) || 0;
      subsetStyle.textContent =
        '@font-face { font-family: "' + family + '"; ' +
        'src: url(data:font/ttf;base64,' + b64 + ') format("truetype"); } ' +
        '@font-palette-values --hbPalette { font-family: "' + family + '"; ' +
        'base-palette: ' + palIdx + '; }';
      subsetPreview.style.fontFamily = '"' + family + '", system-ui, sans-serif';
      subsetPreview.style.fontSize = currentSize () + "px";
      subsetPreview.style.fontVariationSettings = cssVariationSettings ();
      subsetPreview.style.fontPalette = "--hbPalette";
      subsetPreview.textContent = textInput.value;
    });
  }

  /* GPU tab: live iframe of harfbuzz.github.io/hb-gpu-demo.
   * The demo's embed mode accepts { kind: 'text', value } and
   * { kind: 'font', bytes } via postMessage and sends back a
   * { kind: 'ready' } once its wasm runtime is up.  Initial
   * text rides the URL; font bytes have to wait for ready.  */
  const gpuFrame = document.getElementById ("gpu-frame");
  const GPU_ORIGIN = "https://harfbuzz.github.io";
  function postGpu (msg) {
    if (gpuFrame.contentWindow)
      gpuFrame.contentWindow.postMessage (msg, GPU_ORIGIN);
  }
  function gpuFrameUrl () {
    const u = new URL ("https://harfbuzz.github.io/hb-gpu-demo/");
    u.searchParams.set ("embed", "1");
    u.searchParams.set ("text", textInput.value);
    return u.toString ();
  }
  /* When the iframe's runtime is ready it posts { kind: 'ready' }.
   * That's the point at which _web_set_text / _web_load_font
   * are safe to drive via postMessage -- push the current
   * text and font once we hear it. */
  let gpuReady = false;
  window.addEventListener ("message", (e) => {
    if (e.origin !== GPU_ORIGIN) return;
    if (e.data && e.data.kind === "ready") {
      gpuReady = true;
      postGpu ({ kind: "text", value: textInput.value });
      if (fontBuf) postGpu ({ kind: "font", bytes: fontBuf.buffer.slice (0) });
      updateVariations ();
      /* Mirror the host's currently-selected palette into the
       * iframe.  Otherwise switching to the GPU tab after
       * picking a non-zero palette in another tab would render
       * with palette 0 until the user touches the dropdown. */
      const pIdx = parseInt (paletteSelect.value, 10) || 0;
      if (pIdx)
        postGpu ({ kind: "palette", value: pIdx });
      /* First rebuild_buffer on a freshly-loaded font
       * sometimes leaves the atlas half-uploaded and the
       * first composite blank.  A second text push forces
       * another rebuild that fills the atlas properly --
       * the same nudge tab-switching-back happens to
       * perform, and the only thing that reliably rescues
       * this. */
      setTimeout (() => {
        postGpu ({ kind: "text", value: textInput.value });
      }, 200);
    }
  });
  /* Set the iframe src exactly once, lazily on first gpu-tab
   * activation, with current text baked into the URL.  After
   * that, every update flows through postMessage -- setting
   * .src again would reload the iframe and hit all the
   * failure modes of racing the wasm bootstrap. */
  let gpuLoaded = false;
  function renderGpu () {
    if (!gpuLoaded) {
      gpuLoaded = true;
      /* Two-frame delay so both visibility and layout have
       * committed before hb-gpu-demo's GLFW canvas measures
       * itself; on load, dispatch a synthetic resize into
       * the iframe for hb-gpu-demo's resize handler to
       * re-measure the canvas (belt + suspenders). */
      gpuFrame.addEventListener ("load", () => {
        try { gpuFrame.contentWindow.dispatchEvent (new Event ("resize")); } catch {}
      }, { once: true });
      requestAnimationFrame (() => requestAnimationFrame (() => {
        gpuFrame.src = gpuFrameUrl ();
      }));
      return;
    }
    if (gpuReady)
      postGpu ({ kind: "text", value: textInput.value });
    renderSnippet ("gpu");
  }

  const rasterCanvas = document.getElementById ("raster-canvas");
  const rasterCtx    = rasterCanvas.getContext ("2d");
  const rasterStats  = document.getElementById ("raster-stats");
  const rasterDl     = document.getElementById ("raster-dl-png");
  const rasterPngSize = document.getElementById ("raster-png-size");
  let rasterPngUrl   = null;
  function renderRaster () {
    withText ((textPtr) => {
      const wPtr = Module._malloc (4);
      const hPtr = Module._malloc (4);
      /* Render at devicePixelRatio so a 48-px font on a
       * 2x display gets a 96-px pixel buffer that the
       * browser displays at 48 CSS px -- no upscale, no
       * blur. */
      const dpr = window.devicePixelRatio || 1;
      const dataPtr = Module._web_render_raster (fontPtr, fontBuf.length,
                                                  textPtr, currentSize () * dpr,
                                                  wPtr, hPtr);
      if (!dataPtr) {
        Module._free (wPtr); Module._free (hPtr);
        return;
      }
      const w = new Uint32Array (Module.HEAPU8.buffer, wPtr, 1)[0];
      const h = new Uint32Array (Module.HEAPU8.buffer, hPtr, 1)[0];
      const bgra = Module.HEAPU8.slice (dataPtr, dataPtr + w * h * 4);
      Module._web_free_string (dataPtr);
      Module._free (wPtr); Module._free (hPtr);

      /* BGRA premultiplied -> RGBA non-premultiplied in place.
       * ImageData expects non-premultiplied RGBA. */
      for (let i = 0; i < bgra.length; i += 4) {
        const b = bgra[i], g = bgra[i + 1], r = bgra[i + 2], a = bgra[i + 3];
        if (a > 0 && a < 255) {
          bgra[i]     = Math.min (255, (r * 255 / a + 0.5) | 0);
          bgra[i + 1] = Math.min (255, (g * 255 / a + 0.5) | 0);
          bgra[i + 2] = Math.min (255, (b * 255 / a + 0.5) | 0);
        } else {
          bgra[i] = r; bgra[i + 2] = b;
        }
      }
      rasterCanvas.width = w;
      rasterCanvas.height = h;
      /* CSS-display at the un-DPR'd size so the browser
       * maps each pixel to one device pixel. */
      rasterCanvas.style.width = (w / dpr) + "px";
      rasterCanvas.style.height = (h / dpr) + "px";
      const imageData = new ImageData (new Uint8ClampedArray (bgra.buffer), w, h);
      rasterCtx.putImageData (imageData, 0, 0);
      rasterStats.textContent =
        w + " \u00d7 " + h + " px"
        + " (" + Math.round (w / dpr) + " \u00d7 " + Math.round (h / dpr) + " CSS px"
        + " \u00b7 " + (Math.round (dpr * 100) / 100) + "\u00d7 DPR)";
      /* Re-encode the canvas as PNG for the download link.
       * toBlob is async; the previous URL is revoked once the
       * new one lands so we don't leak. */
      rasterCanvas.toBlob ((blob) => {
        if (!blob) return;
        if (rasterPngUrl) URL.revokeObjectURL (rasterPngUrl);
        rasterPngUrl = URL.createObjectURL (blob);
        rasterDl.href = rasterPngUrl;
        rasterPngSize.textContent = fmtBytes (blob.size);
      }, "image/png");
    });
    renderSnippet ("raster");
  }

  /* Static tabs (embed, subset, gpu) have no live render --
   * their content is inert HTML.  They still get an entry so
   * activation/hash routing is uniform. */
  const noop = () => {};
  const demos = {
    embed:  { section: document.getElementById ("demo-embed"),  render: noop          },
    shape:  { section: document.getElementById ("demo-shape"),  render: renderShape   },
    subset: { section: document.getElementById ("demo-subset"), render: renderSubset  },
    raster: { section: document.getElementById ("demo-raster"), render: renderRaster  },
    vector: { section: document.getElementById ("demo-vector"), render: renderVector  },
    gpu:    { section: document.getElementById ("demo-gpu"),    render: renderGpu     },
  };
  const tabs = document.querySelectorAll (".tab");

  let activeName = null;
  function activate (name) {
    if (!demos[name]) name = "embed";
    if (name === activeName) return;
    activeName = name;
    document.body.dataset.active = name;
    document.title = name === "embed"
      ? "harfbuzz-world.cc — your one-stop HarfBuzz shop"
      : name + " — harfbuzz-world.cc";
    for (const [n, d] of Object.entries (demos))
      d.section.hidden = (n !== name);
    for (const t of tabs)
      t.classList.toggle ("active", t.dataset.demo === name);
    const logoMap = { embed: "hb-world.png", shape: "hb-shape.png",
                      subset: "hb-subset.png", raster: "hb-raster.png",
                      vector: "hb-vector.png", gpu: "hb-gpu.png" };
    const logo = document.getElementById ("site-logo");
    if (logo) {
      const newSrc = logoMap[name] || "hb-world.png";
      if (!logo.src.endsWith (newSrc)) {
        const next = logo.cloneNode (false);
        next.src = newSrc;
        next.classList.add ("logo-out");
        next.id = "";
        logo.parentNode.appendChild (next);
        next.offsetHeight; /* force reflow */
        next.classList.remove ("logo-out");
        logo.classList.add ("logo-out");
        setTimeout (() => {
          logo.remove ();
          next.id = "site-logo";
        }, 350);
      }
    }
    demos[name].render ();
  }

  function renderActive () {
    if (activeName) demos[activeName].render ();
  }

  function fromHash () {
    const h = (location.hash || "").replace (/^#/, "");
    const parts = h.split ("/");
    const tab = parts[0] || "embed";
    const sub = parts[1] || "";
    activate (tab);
    /* Open a sub-section if the hash requests it. */
    let scrollTarget = null;
    if (sub === "code") {
      applySnippetOpen (true);
      scrollTarget = document.querySelector ("#demo-" + tab + " details[data-snippet]");
    } else if (sub === "tables") {
      const tw = document.getElementById ("subset-tables-wrap");
      if (tw) { tw.hidden = false; tw.open = true; scrollTarget = tw; }
    } else if (sub) {
      const el = document.querySelector ("#demo-" + tab + " details[data-section=\"" + sub + "\"]");
      if (el) { el.open = true; scrollTarget = el; }
    }
    if (scrollTarget)
      setTimeout (() => scrollTarget.scrollIntoView ({ behavior: "smooth", block: "start" }), 100);
  }
  window.addEventListener ("hashchange", fromHash);

  /* Logo click: let the <a href="./"> reload to bare URL.
   * Full reload, all query params and hash dropped, all
   * widget state back to defaults.  No JS interception. */
  /* Reflect current text/size in the URL so the view is
   * shareable.  Debounced so we don't replaceState per
   * keystroke. */
  let urlSyncTimer = 0;
  function syncUrl () {
    clearTimeout (urlSyncTimer);
    urlSyncTimer = setTimeout (() => {
      const url = new URL (location.href);
      const cur = url.searchParams.get ("preset");
      /* The "default" text depends on context: a preset's
       * own text if a preset is selected, otherwise the
       * site-wide default (hello-world!).  Only emit ?text
       * when the user has typed something different. */
      const defaultText = (cur && PRESETS[cur]) ? PRESETS[cur].text : "hello-world!";
      if (textInput.value && textInput.value !== defaultText)
        url.searchParams.set ("text", textInput.value);
      else
        url.searchParams.delete ("text");
      if (sizeInput.value && sizeInput.value !== "72") url.searchParams.set ("size", sizeInput.value);
      else                                             url.searchParams.delete ("size");
      /* Only touch ?variations when the current font has
       * sliders to read.  If we're on a font with no axes
       * (e.g. the emoji preset), leave any prior
       * ?variations alone -- the user might switch back to
       * a variable font and want them restored. */
      if (currentAxes.length) {
        const v = variationsForUrl ();
        if (v) url.searchParams.set ("variations", v);
        else   url.searchParams.delete ("variations");
      }
      /* Same convention as variations: only touch ?palette
       * when the current font actually has multi-palette,
       * so a font switch doesn't clobber a prior selection. */
      if (!paletteLabel.hidden) {
        const pIdx = parseInt (paletteSelect.value, 10) || 0;
        if (pIdx) url.searchParams.set ("palette", String (pIdx));
        else      url.searchParams.delete ("palette");
      }
      /* Preset stays put even when text/variations diverge --
       * the pill represents the script + font choice and
       * widget tweaks layer on top. */
      history.replaceState (null, "", url);
      reflectActivePreset ();
    }, 200);
  }
  textInput.addEventListener ("input", () => { renderActive (); syncUrl (); });
  sizeInput.addEventListener ("input", () => { renderActive (); syncUrl (); });

  /* Variable axes: pull fvar info from the current font,
   * render one range slider per axis, and push the combined
   * variations string into wasm + the gpu iframe on every
   * drag.  Axes without a slider fall back to the axis
   * default. */
  const axesEl = document.getElementById ("var-axes");
  let currentAxes = [];  /* [{tag, min, def, max, name, slider, value}, ...] */
  function variationsString () {
    return currentAxes
      .map ((a) => a.tag + "=" + a.value)
      .join (",");
  }
  function cssVariationSettings () {
    return currentAxes
      .map ((a) => '"' + a.tag + '" ' + a.value)
      .join (", ");
  }
  function updateVariations () {
    const s = variationsString ();
    const buf = Module._malloc (s.length + 1);
    Module.stringToUTF8 (s, buf, s.length + 1);
    Module._web_set_variations (buf);
    Module._free (buf);
    if (gpuReady)
      postGpu ({ kind: "variations", value: s });
    /* Reflect non-default variations in the URL so the
     * view is shareable.  Skip when sliders are still at
     * the font's defaults (no point cluttering URL). */
    syncUrl ();
    renderActive ();
  }
  function variationsForUrl () {
    /* Only include axes the user moved off the slider
     * default we picked at refresh time. */
    return currentAxes
      .filter ((a) => a.value !== a.startValue)
      .map ((a) => a.tag + "=" + a.value)
      .join (",");
  }
  function refreshAxes () {
    const ptr = Module._web_font_axes (fontPtr, fontBuf.length);
    const axes = JSON.parse (Module.UTF8ToString (ptr));
    Module._web_free_string (ptr);
    axesEl.innerHTML = "";
    currentAxes = axes.map ((a) => {
      const row = document.createElement ("label");
      row.className = "axis";
      const caption = document.createElement ("span");
      caption.className = "axis-caption";
      caption.textContent = a.name || a.tag;
      const slider = document.createElement ("input");
      slider.type = "range";
      slider.min = a.min;
      slider.max = a.max;
      slider.step = (a.max - a.min) / 100;
      /* Start wght at Regular (400) regardless of the font's
       * own fvar default -- CJK families tend to pick
       * wght=100 as default which renders as Thin. */
      const startValue = a.tag === "wght"
        ? Math.min (a.max, Math.max (a.min, 400))
        : a.def;
      slider.value = startValue;
      const readout = document.createElement ("span");
      readout.className = "axis-value";
      readout.textContent = String (startValue);
      row.append (caption, slider, readout);
      axesEl.append (row);
      const entry = { tag: a.tag, name: a.name, min: a.min, def: a.def,
                      max: a.max, slider, readout, value: startValue,
                      startValue };
      slider.addEventListener ("input", () => {
        entry.value = parseFloat (slider.value);
        readout.textContent = (+slider.value).toFixed (2).replace (/\.?0+$/, "");
        updateVariations ();
      });
      return entry;
    });
    axesEl.hidden = currentAxes.length === 0;
    /* Subset's "Instantiate variations" toggle only matters
     * for variable fonts; hide it when there are no axes. */
    subsetInstantiateLabel.hidden = currentAxes.length === 0;
    /* If the URL carries an explicit variations= setting,
     * apply it to the sliders before pushing wasm + GPU
     * state.  Lets a shared link reproduce the view. */
    const urlVars = new URLSearchParams (location.search).get ("variations");
    if (urlVars)
      urlVars.split (",").forEach ((pair) => {
        const [tag, val] = pair.split ("=");
        const axis = currentAxes.find ((a) => a.tag === tag);
        if (axis && val !== undefined) {
          const v = parseFloat (val);
          axis.value = v;
          axis.slider.value = v;
          axis.readout.textContent = String (v);
        }
      });
    updateVariations ();
  }

  /* CPAL palettes: pull the list from the current font and
   * surface a dropdown when there are two or more palettes.
   * Single-palette fonts (most COLR fonts) and non-color fonts
   * stay quiet. */
  function paletteLabelFor (p, i) {
    if (p.name) return p.name;
    if (p.flags & 1) return "Light background";
    if (p.flags & 2) return "Dark background";
    return "Palette " + i;
  }
  function refreshPalettes () {
    const ptr = Module._web_font_palettes (fontPtr, fontBuf.length);
    const palettes = JSON.parse (Module.UTF8ToString (ptr));
    Module._web_free_string (ptr);
    paletteSelect.innerHTML = "";
    Module._web_set_palette (0);
    if (gpuReady)
      postGpu ({ kind: "palette", value: 0 });
    if (palettes.length < 2) {
      paletteLabel.hidden = true;
      return;
    }
    palettes.forEach ((p, i) => {
      const opt = document.createElement ("option");
      opt.value = i;
      opt.textContent = paletteLabelFor (p, i);
      paletteSelect.append (opt);
    });
    /* Honour ?palette=N from the URL so a shared link can pin
     * the picker.  Falls back to 0 (and any out-of-range index
     * also lands on 0) so we never load with an invalid pick. */
    const urlPal = parseInt (new URLSearchParams (location.search).get ("palette"), 10);
    const startIdx = (urlPal >= 0 && urlPal < palettes.length) ? urlPal : 0;
    paletteSelect.value = String (startIdx);
    if (startIdx) {
      Module._web_set_palette (startIdx);
      if (gpuReady)
        postGpu ({ kind: "palette", value: startIdx });
    }
    paletteLabel.hidden = false;
  }
  paletteSelect.addEventListener ("change", () => {
    const idx = parseInt (paletteSelect.value, 10) || 0;
    Module._web_set_palette (idx);
    if (gpuReady)
      postGpu ({ kind: "palette", value: idx });
    renderActive ();
    syncUrl ();
  });

  /* Presets: one-click combos of text + font, covering the
   * three scripts we ship fonts for. */
  /* PRESETS is defined in js/presets.js, loaded before this script. */
  function applyPreset (key) {
    const p = PRESETS[key];
    if (!p) return false;
    textInput.value = p.text;
    /* If the user has loaded a custom font (URL / file / GF),
     * presets become text-only so they can compare scripts
     * across the same chosen font.  Otherwise the preset
     * brings its bundled script-appropriate font along. */
    const url = new URL (location.href);
    url.searchParams.delete ("text");
    url.searchParams.delete ("size");
    url.searchParams.set ("preset", key);
    if (customFontActive) {
      /* Keep ?font; just re-render with the new text. */
      renderActive ();
    } else {
      url.searchParams.delete ("font");
      loadFontUrl (p.font, p.name, { silentUrl: true, preset: true });
    }
    history.replaceState (null, "", url);
    reflectActivePreset ();
    return true;
  }
  const presetButtons = document.querySelectorAll (".preset");
  presetButtons.forEach ((btn) => {
    btn.addEventListener ("click", () => applyPreset (btn.dataset.preset));
  });

  /* Highlight the preset button currently reflected in the
   * URL -- but only when URL is a "clean" preset state (no
   * ?text / ?font overrides).  Gets called from syncUrl and
   * applyPreset so it stays in sync with location.search. */
  function reflectActivePreset () {
    const key = new URLSearchParams (location.search).get ("preset");
    presetButtons.forEach ((btn) => {
      btn.classList.toggle ("active", btn.dataset.preset === key);
    });
  }

  /* Font picker: dropdown menu with three sources (shipped /
   * file / URL), plus drag-and-drop anywhere on the page. */
  async function loadFontFile (file) {
    const bytes = new Uint8Array (await file.arrayBuffer ());
    const name = file.name.replace (/\.(ttf|otf|ttc|woff2?)$/i, "");
    setFontBytes (bytes, name);
    customFontActive = true;
    /* Cache in IndexedDB so the font survives page refresh.
     * Put font=@<hash> in the URL so the reload path can
     * look it up. */
    const u = new URL (location.href);
    try {
      const hash = await fontHash (bytes);
      const db = await fontDbOpen ();
      await fontDbPut (db, hash, { bytes, name });
      u.searchParams.set ("font", "@" + hash);
    } catch {
      u.searchParams.delete ("font");
    }
    history.replaceState (null, "", u);
    reflectActivePreset ();
  }

  function openFontMenu () { fontMenu.hidden = false; }
  function closeFontMenu () { fontMenu.hidden = true; }
  fontButton.addEventListener ("click", () => {
    fontMenu.hidden ? openFontMenu () : closeFontMenu ();
  });
  /* Close on click outside the picker.  Use the picker root
   * (button + menu) so clicks inside the menu -- including on
   * native <select> popups -- don't immediately re-close. */
  const fontPicker = fontButton.parentElement;
  document.addEventListener ("click", (e) => {
    if (!fontMenu.hidden && !fontPicker.contains (e.target))
      closeFontMenu ();
  });
  document.addEventListener ("keydown", (e) => {
    if (e.key === "Escape") closeFontMenu ();
  });

  fontShipped.addEventListener ("change", () => {
    const opt = fontShipped.selectedOptions[0];
    /* Treat shipped picks like presets for the custom-font
     * tracker -- they're a curated default, not a "user
     * brought their own" font. */
    loadFontUrl (opt.value, opt.dataset.name, { preset: true });
    closeFontMenu ();
  });
  fontInput.addEventListener ("change", () => {
    if (fontInput.files.length) {
      loadFontFile (fontInput.files[0]);
      closeFontMenu ();
    }
  });
  fontUrlLoad.addEventListener ("click", () => {
    if (fontUrl.value) {
      loadFontUrl (fontUrl.value);
      closeFontMenu ();
    }
  });
  fontUrl.addEventListener ("keydown", (e) => {
    if (e.key === "Enter") fontUrlLoad.click ();
  });

  /* Google Fonts picker.  We bypass the woff2-only css2 API by
   * reading google/fonts' own family_features.json (used by
   * their familyexplorer.html), which lists each family's TTF
   * relative path.  Fetched lazily on first focus so the
   * ~380KB JSON only downloads when someone actually opens the
   * picker; cached for the session. */
  const GF_RAW = "https://raw.githubusercontent.com/google/fonts/main/";
  const GF_META = GF_RAW + ".ci/family_features.json";
  let gfFamiliesPromise = null;
  function fetchGfFamilies () {
    if (gfFamiliesPromise) return gfFamiliesPromise;
    gfFamiliesPromise = fetch (GF_META)
      .then ((r) => { if (!r.ok) throw new Error ("HTTP " + r.status); return r.json (); })
      .then ((data) => data.families || {})
      .catch ((e) => { gfFamiliesPromise = null; throw e; });
    return gfFamiliesPromise;
  }
  let gfDatalistPopulated = false;
  fontGf.addEventListener ("focus", async () => {
    if (gfDatalistPopulated) return;
    try {
      const families = await fetchGfFamilies ();
      const frag = document.createDocumentFragment ();
      Object.keys (families).sort ().forEach ((name) => {
        const opt = document.createElement ("option");
        opt.value = name;
        frag.append (opt);
      });
      fontGfList.append (frag);
      gfDatalistPopulated = true;
    } catch (e) { /* leave empty; user can still type a known family */ }
  });
  async function loadGfFamily (name) {
    name = name.trim ();
    if (!name) return;
    try {
      const families = await fetchGfFamilies ();
      const entry = families[name];
      if (!entry || !entry.fp) {
        fontGf.setCustomValidity ("Unknown family");
        fontGf.reportValidity ();
        setTimeout (() => fontGf.setCustomValidity (""), 2000);
        return;
      }
      const url = GF_RAW + entry.fp.replace (/^\.\//, "");
      const ok = await loadFontUrl (url, name);
      if (ok) closeFontMenu ();
    } catch { /* network/json error: silent */ }
  }
  fontGfLoad.addEventListener ("click", () => loadGfFamily (fontGf.value));
  fontGf.addEventListener ("keydown", (e) => {
    if (e.key === "Enter") fontGfLoad.click ();
  });

  /* Drag-and-drop only takes effect when the font picker is
   * open; otherwise normal page-drag behavior (text selection,
   * link drag, etc.) keeps working without our overlay
   * blanking the screen. */
  document.addEventListener ("dragover", (e) => {
    if (fontMenu.hidden) return;
    e.preventDefault ();
    dropOverlay.classList.add ("active");
  });
  document.addEventListener ("dragleave", (e) => {
    if (e.target === document || e.relatedTarget === null)
      dropOverlay.classList.remove ("active");
  });
  document.addEventListener ("drop", (e) => {
    if (fontMenu.hidden) return;
    e.preventDefault ();
    dropOverlay.classList.remove ("active");
    if (e.dataTransfer.files.length) loadFontFile (e.dataTransfer.files[0]);
  });

  /* Load a font from a URL.  Used both for the bundled default
   * and for the ?font=URL query parameter.  Returns true on
   * success so the caller can fall back. */
  /* Tracks whether the active font came from a "custom" source
   * (URL / file drop / Google Fonts).  When true, preset
   * buttons become text-only so the user can sweep scripts
   * across the same custom font.  Reset to false on the
   * site-default load and on shipped <select> + preset picks. */
  let customFontActive = false;
  async function loadFontUrl (url, displayName, opts) {
    try {
      const r = await fetch (url);
      if (!r.ok) return false;
      const bytes = new Uint8Array (await r.arrayBuffer ());
      const name = displayName || url.replace (/^.*\//, "")
                                     .replace (/\.(ttf|otf|ttc|woff2?)$/i, "");
      setFontBytes (bytes, name);
      customFontActive = !(opts && opts.preset);
      /* The file input remembers its last selection by value, so
       * picking the same file twice in a row never fires
       * 'change'.  Clearing it here lets the user re-upload the
       * same file after a preset / URL load took control of the
       * active font. */
      fontInput.value = "";
      /* Sync the shipped-fonts <select> if @url matches one
       * of its options, so the dropdown is honest about the
       * current font.  Falls through silently for ad-hoc
       * URL / file picks, which the dropdown can't represent. */
      for (const opt of fontShipped.options)
        if (opt.value === url) { fontShipped.value = url; break; }
      /* Reflect the font URL in the location bar.  Skip when
       * called from applyPreset / initial ?font= load, which
       * own URL state themselves.  Keep any ?preset= intact:
       * changing just the font doesn't invalidate the
       * preset's text, only a text change does. */
      if (!opts || !opts.silentUrl) {
        const u = new URL (location.href);
        u.searchParams.set ("font", url);
        history.replaceState (null, "", u);
    reflectActivePreset ();
      }
      return true;
    } catch { return false; }
  }

  /* Try to restore a cached font from IndexedDB.
   * Returns true if font=@hash was found and loaded. */
  async function loadFontFromCache (hash) {
    try {
      const db = await fontDbOpen ();
      const entry = await fontDbGet (db, hash);
      if (!entry) return false;
      setFontBytes (new Uint8Array (entry.bytes), entry.name);
      customFontActive = true;
      return true;
    } catch { return false; }
  }

  /* Initial state.  Priority: ?preset=<name> > ?font=@hash >
   * ?font=URL > bundled NotoSans.  ?preset wins because it owns
   * both text and font, so a preset link reproduces the view. */
  const params = new URLSearchParams (location.search);
  const textParam = params.get ("text");
  const sizeParam = params.get ("size");
  const presetParam = params.get ("preset");
  const fontUrlParam = params.get ("font");
  if (textParam !== null) textInput.value = textParam;
  if (sizeParam !== null) sizeInput.value = sizeParam;
  if (presetParam && PRESETS[presetParam]) {
    if (textParam === null) textInput.value = PRESETS[presetParam].text;
    const fontChoice = fontUrlParam || PRESETS[presetParam].font;
    const nameChoice = fontUrlParam ? null : PRESETS[presetParam].name;
    if (fontUrlParam && fontUrlParam.startsWith ("@"))
      await loadFontFromCache (fontUrlParam.slice (1));
    else
      await loadFontUrl (fontChoice, nameChoice,
                         fontUrlParam ? { silentUrl: true }
                                      : { silentUrl: true, preset: true });
  } else if (fontUrlParam && fontUrlParam.startsWith ("@")) {
    if (!(await loadFontFromCache (fontUrlParam.slice (1))))
      await loadFontUrl ("fonts/NotoSans.ttf", "NotoSans", { silentUrl: true, preset: true });
  } else if (!fontUrlParam || !(await loadFontUrl (fontUrlParam, null, { silentUrl: true })))
    await loadFontUrl ("fonts/NotoSans.ttf", "NotoSans", { silentUrl: true, preset: true });

  reflectActivePreset ();
  fromHash ();
}) ();
