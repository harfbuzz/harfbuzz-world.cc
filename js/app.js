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

  const demos = {
    shape:  { section: document.getElementById ("demo-shape"),  render: renderShape  },
    vector: { section: document.getElementById ("demo-vector"), render: renderVector },
    raster: { section: document.getElementById ("demo-raster"), render: renderRaster },
  };
  const tabs = document.querySelectorAll (".tab");

  let activeName = null;
  function activate (name) {
    if (!demos[name]) name = "shape";
    if (name === activeName) return;
    activeName = name;
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
    activate (h || "shape");
  }
  window.addEventListener ("hashchange", fromHash);
  textInput.addEventListener ("input", renderActive);
  sizeInput.addEventListener ("input", renderActive);

  /* Font picker: dropdown menu with three sources (shipped /
   * file / URL), plus drag-and-drop anywhere on the page. */
  async function loadFontFile (file) {
    const bytes = new Uint8Array (await file.arrayBuffer ());
    const name = file.name.replace (/\.(ttf|otf|ttc|woff2?)$/i, "");
    setFontBytes (bytes, name);
  }

  function openFontMenu () { fontMenu.hidden = false; }
  function closeFontMenu () { fontMenu.hidden = true; }
  fontButton.addEventListener ("click", (e) => {
    e.stopPropagation ();
    fontMenu.hidden ? openFontMenu () : closeFontMenu ();
  });
  fontMenu.addEventListener ("click", (e) => e.stopPropagation ());
  document.addEventListener ("click", closeFontMenu);
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

  /* Initial font: ?font=URL if present, else bundled NotoSans.
   * On URL failure, fall back to the bundled default so the
   * page is never left fontless. */
  const params = new URLSearchParams (location.search);
  const fontUrlParam = params.get ("font");
  if (!fontUrlParam || !(await loadFontUrl (fontUrlParam)))
    await loadFontUrl ("fonts/NotoSans-Regular.ttf", "NotoSans-Regular");

  fromHash ();
}) ();
