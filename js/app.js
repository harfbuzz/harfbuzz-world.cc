/* HarfBuzz World — single-page demo shell.
 *
 * Loads wasm + the default font once, then routes the shared
 * text/size controls to whichever demo is currently active.
 * Active demo is driven by the URL hash (#shape, #vector,
 * #raster) so links and back/forward navigation work. */

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
    if (displayName) fontNameEl.textContent = displayName;
    /* Also push to the GPU iframe if its runtime is up. */
    if (gpuReady)
      postGpu ({ kind: "font", bytes: fontBuf.buffer.slice (0) });
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
  const fontNameEl    = document.getElementById ("font-name");
  const dropOverlay   = document.getElementById ("drop-overlay");

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
    return parseFloat (sizeInput.value) || 48;
  }

  /* Demos.  Each exposes a render() that reads the shared
   * controls and updates its DOM. */

  const shapeRender  = document.getElementById ("shape-render");
  const shapeGlyphs  = document.getElementById ("shape-glyphs");
  function renderShape () {
    withText ((textPtr) => {
      const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                              textPtr, currentSize ());
      shapeRender.innerHTML = Module.UTF8ToString (svgPtr);
      Module._web_free_string (svgPtr);

      const jsonPtr = Module._web_shape_json (fontPtr, fontBuf.length, textPtr);
      shapeGlyphs.textContent = JSON.stringify (
        JSON.parse (Module.UTF8ToString (jsonPtr)), null, 2);
      Module._web_free_string (jsonPtr);
    });
  }

  const vectorRender = document.getElementById ("vector-render");
  const dlSvg        = document.getElementById ("vector-dl-svg");
  const dlPdf        = document.getElementById ("vector-dl-pdf");
  let svgUrl = null, pdfUrl = null;
  function renderVector () {
    withText ((textPtr) => {
      const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                              textPtr, currentSize ());
      const svg = Module.UTF8ToString (svgPtr);
      Module._web_free_string (svgPtr);
      vectorRender.innerHTML = svg;
      if (svgUrl) URL.revokeObjectURL (svgUrl);
      svgUrl = URL.createObjectURL (new Blob ([svg], { type: "image/svg+xml" }));
      dlSvg.href = svgUrl;

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
    });
  }

  const subsetOrig    = document.getElementById ("subset-orig-size");
  const subsetNew     = document.getElementById ("subset-new-size");
  const subsetSaving  = document.getElementById ("subset-saving");
  const subsetDl      = document.getElementById ("subset-download");
  const subsetPreview = document.getElementById ("subset-preview");
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

      subsetNew.textContent = fmtBytes (sublen);
      const pct = (100 * (1 - sublen / fontBuf.length)).toFixed (1);
      subsetSaving.textContent = pct + "% (" + fmtBytes (fontBuf.length - sublen) + ")";

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
      subsetStyle.textContent =
        '@font-face { font-family: "' + family + '"; ' +
        'src: url(data:font/ttf;base64,' + b64 + ') format("truetype"); }';
      subsetPreview.style.fontFamily = '"' + family + '", system-ui, sans-serif';
      subsetPreview.style.fontSize = currentSize () + "px";
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
  function focusGpu () {
    try { gpuFrame.contentWindow.focus (); } catch {}
    try { gpuFrame.focus (); } catch {}
  }
  function renderGpu () {
    setTimeout (focusGpu, 0);
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
  }

  const rasterCanvas = document.getElementById ("raster-canvas");
  const rasterCtx    = rasterCanvas.getContext ("2d");
  function renderRaster () {
    withText ((textPtr) => {
      const wPtr = Module._malloc (4);
      const hPtr = Module._malloc (4);
      const dataPtr = Module._web_render_raster (fontPtr, fontBuf.length,
                                                  textPtr, currentSize (),
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

      /* BGRA -> RGBA in place. */
      for (let i = 0; i < bgra.length; i += 4) {
        const b = bgra[i], r = bgra[i + 2];
        bgra[i] = r; bgra[i + 2] = b;
      }
      rasterCanvas.width = w;
      rasterCanvas.height = h;
      const imageData = new ImageData (new Uint8ClampedArray (bgra.buffer), w, h);
      rasterCtx.putImageData (imageData, 0, 0);
    });
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
    for (const [n, d] of Object.entries (demos))
      d.section.hidden = (n !== name);
    for (const t of tabs)
      t.classList.toggle ("active", t.dataset.demo === name);
    demos[name].render ();
  }

  function renderActive () {
    if (activeName) demos[activeName].render ();
  }

  function fromHash () {
    const h = (location.hash || "").replace (/^#/, "");
    activate (h || "embed");
  }
  window.addEventListener ("hashchange", fromHash);

  /* Logo click: go to the embed ("home") tab and clear the
   * hash from the URL -- without a full page reload. */
  document.getElementById ("logo-home").addEventListener ("click", (e) => {
    e.preventDefault ();
    history.pushState (null, "", location.pathname + location.search);
    activate ("embed");
  });
  /* Reflect current text/size in the URL so the view is
   * shareable.  Debounced so we don't replaceState per
   * keystroke. */
  let urlSyncTimer = 0;
  function syncUrl () {
    clearTimeout (urlSyncTimer);
    urlSyncTimer = setTimeout (() => {
      const url = new URL (location.href);
      if (textInput.value) url.searchParams.set ("text", textInput.value);
      else                 url.searchParams.delete ("text");
      if (sizeInput.value && sizeInput.value !== "48") url.searchParams.set ("size", sizeInput.value);
      else                                             url.searchParams.delete ("size");
      url.searchParams.delete ("preset");
      history.replaceState (null, "", url);
    }, 200);
  }
  textInput.addEventListener ("input", () => { renderActive (); syncUrl (); });
  sizeInput.addEventListener ("input", () => { renderActive (); syncUrl (); });

  /* Presets: one-click combos of text + font, covering the
   * three scripts we ship fonts for. */
  const PRESETS = {
    latin:      { text: "hello-world!",      font: "fonts/NotoSans-Regular.ttf",   name: "NotoSans-Regular" },
    arabic:     { text: "مرحبا بالعالم",      font: "fonts/NotoSansArabic.ttf",     name: "NotoSansArabic" },
    devanagari: { text: "नमस्ते दुनिया",       font: "fonts/NotoSansDevanagari.ttf", name: "NotoSansDevanagari" },
    emoji:      { text: "😋😎😍😘🥰",         font: "fonts/NotoEmoji.ttf",          name: "NotoEmoji" },
  };
  function applyPreset (key) {
    const p = PRESETS[key];
    if (!p) return false;
    textInput.value = p.text;
    loadFontUrl (p.font, p.name);
    /* Reflect the choice in the URL so the view is
     * shareable; keep the hash (active tab) untouched. */
    const url = new URL (location.href);
    url.searchParams.set ("preset", key);
    history.replaceState (null, "", url);
    return true;
  }
  document.querySelectorAll (".preset").forEach ((btn) => {
    btn.addEventListener ("click", () => applyPreset (btn.dataset.preset));
  });

  /* Font picker: dropdown menu with three sources (shipped /
   * file / URL), plus drag-and-drop anywhere on the page. */
  async function loadFontFile (file) {
    const bytes = new Uint8Array (await file.arrayBuffer ());
    const name = file.name.replace (/\.(ttf|otf|ttc|woff2?)$/i, "");
    setFontBytes (bytes, name);
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
    loadFontUrl (opt.value, opt.dataset.name);
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

  document.addEventListener ("dragover", (e) => {
    e.preventDefault ();
    dropOverlay.classList.add ("active");
  });
  document.addEventListener ("dragleave", (e) => {
    if (e.target === document || e.relatedTarget === null)
      dropOverlay.classList.remove ("active");
  });
  document.addEventListener ("drop", (e) => {
    e.preventDefault ();
    dropOverlay.classList.remove ("active");
    if (e.dataTransfer.files.length) loadFontFile (e.dataTransfer.files[0]);
  });

  /* Load a font from a URL.  Used both for the bundled default
   * and for the ?font=URL query parameter.  Returns true on
   * success so the caller can fall back. */
  async function loadFontUrl (url, displayName) {
    try {
      const r = await fetch (url);
      if (!r.ok) return false;
      const bytes = new Uint8Array (await r.arrayBuffer ());
      const name = displayName || url.replace (/^.*\//, "")
                                     .replace (/\.(ttf|otf|ttc|woff2?)$/i, "");
      setFontBytes (bytes, name);
      return true;
    } catch { return false; }
  }

  /* Initial state.  Priority: ?preset=<name> > ?font=URL >
   * bundled NotoSans.  ?preset wins because it owns both
   * text and font, so a preset link reproduces the view. */
  const params = new URLSearchParams (location.search);
  const textParam = params.get ("text");
  const sizeParam = params.get ("size");
  const presetParam = params.get ("preset");
  const fontUrlParam = params.get ("font");
  if (textParam !== null) textInput.value = textParam;
  if (sizeParam !== null) sizeInput.value = sizeParam;
  if (presetParam && PRESETS[presetParam])
    applyPreset (presetParam);
  else if (!fontUrlParam || !(await loadFontUrl (fontUrlParam)))
    await loadFontUrl ("fonts/NotoSans-Regular.ttf", "NotoSans-Regular");

  fromHash ();
}) ();
