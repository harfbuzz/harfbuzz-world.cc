/* shape demo: load wasm + a font, render shaped text as SVG. */

(async function main () {
  const Module = await createHbWorld ();

  const fontResp = await fetch ("fonts/NotoSans-Regular.ttf");
  const fontBuf = new Uint8Array (await fontResp.arrayBuffer ());

  /* Allocate a wasm-side copy of the font bytes once. */
  const fontPtr = Module._malloc (fontBuf.length);
  Module.HEAPU8.set (fontBuf, fontPtr);

  const textInput = document.getElementById ("text");
  const sizeInput = document.getElementById ("size");
  const renderEl  = document.getElementById ("render");
  const glyphsEl  = document.getElementById ("glyphs");

  function render () {
    const text = textInput.value;
    const size = parseFloat (sizeInput.value) || 48;

    /* Marshal text into wasm memory. */
    const textLen = Module.lengthBytesUTF8 (text) + 1;
    const textPtr = Module._malloc (textLen);
    Module.stringToUTF8 (text, textPtr, textLen);

    /* SVG render. */
    const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                            textPtr, size);
    const svg = Module.UTF8ToString (svgPtr);
    Module._web_free_string (svgPtr);

    /* Glyph table. */
    const jsonPtr = Module._web_shape_json (fontPtr, fontBuf.length, textPtr);
    const json = Module.UTF8ToString (jsonPtr);
    Module._web_free_string (jsonPtr);

    Module._free (textPtr);

    renderEl.innerHTML = svg;
    glyphsEl.textContent = JSON.stringify (JSON.parse (json), null, 2);
  }

  textInput.addEventListener ("input", render);
  sizeInput.addEventListener ("input", render);
  render ();
}) ();
