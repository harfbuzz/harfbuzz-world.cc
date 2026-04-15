/* vector demo: render shaped text as SVG (inline) and PDF (download). */

(async function main () {
  const Module = await createHbWorld ();
  const fontResp = await fetch ("fonts/NotoSans-Regular.ttf");
  const fontBuf = new Uint8Array (await fontResp.arrayBuffer ());
  const fontPtr = Module._malloc (fontBuf.length);
  Module.HEAPU8.set (fontBuf, fontPtr);

  const textInput  = document.getElementById ("text");
  const sizeInput  = document.getElementById ("size");
  const renderEl   = document.getElementById ("render");
  const dlSvg      = document.getElementById ("download-svg");
  const dlPdf      = document.getElementById ("download-pdf");

  let svgUrl = null, pdfUrl = null;

  function render () {
    const text = textInput.value;
    const size = parseFloat (sizeInput.value) || 48;

    const textLen = Module.lengthBytesUTF8 (text) + 1;
    const textPtr = Module._malloc (textLen);
    Module.stringToUTF8 (text, textPtr, textLen);

    /* SVG: inline preview + download. */
    const svgPtr = Module._web_render_svg (fontPtr, fontBuf.length,
                                            textPtr, size);
    const svg = Module.UTF8ToString (svgPtr);
    Module._web_free_string (svgPtr);
    renderEl.innerHTML = svg;
    if (svgUrl) URL.revokeObjectURL (svgUrl);
    svgUrl = URL.createObjectURL (new Blob ([svg], { type: "image/svg+xml" }));
    dlSvg.href = svgUrl;

    /* PDF: download only (binary). */
    const lenPtr = Module._malloc (4);
    const pdfPtr = Module._web_render_pdf (fontPtr, fontBuf.length,
                                            textPtr, size, lenPtr);
    const pdfLen = new Uint32Array (Module.HEAPU8.buffer, lenPtr, 1)[0];
    const pdfBytes = Module.HEAPU8.slice (pdfPtr, pdfPtr + pdfLen);
    Module._web_free_string (pdfPtr);
    Module._free (lenPtr);
    if (pdfUrl) URL.revokeObjectURL (pdfUrl);
    pdfUrl = URL.createObjectURL (new Blob ([pdfBytes], { type: "application/pdf" }));
    dlPdf.href = pdfUrl;

    Module._free (textPtr);
  }

  textInput.addEventListener ("input", render);
  sizeInput.addEventListener ("input", render);
  render ();
}) ();
