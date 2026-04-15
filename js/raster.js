/* raster demo: render to BGRA32 pixels, blit to canvas. */

(async function main () {
  const Module = await createHbWorld ();
  const fontResp = await fetch ("fonts/NotoSans-Regular.ttf");
  const fontBuf = new Uint8Array (await fontResp.arrayBuffer ());
  const fontPtr = Module._malloc (fontBuf.length);
  Module.HEAPU8.set (fontBuf, fontPtr);

  const textInput = document.getElementById ("text");
  const sizeInput = document.getElementById ("size");
  const canvas    = document.getElementById ("canvas");
  const ctx       = canvas.getContext ("2d");

  function render () {
    const text = textInput.value;
    const size = parseFloat (sizeInput.value) || 64;

    const textLen = Module.lengthBytesUTF8 (text) + 1;
    const textPtr = Module._malloc (textLen);
    Module.stringToUTF8 (text, textPtr, textLen);

    const wPtr = Module._malloc (4);
    const hPtr = Module._malloc (4);
    const dataPtr = Module._web_render_raster (fontPtr, fontBuf.length,
                                                textPtr, size, wPtr, hPtr);
    if (!dataPtr) {
      Module._free (wPtr); Module._free (hPtr); Module._free (textPtr);
      return;
    }
    const w = new Uint32Array (Module.HEAPU8.buffer, wPtr, 1)[0];
    const h = new Uint32Array (Module.HEAPU8.buffer, hPtr, 1)[0];
    const bgra = Module.HEAPU8.slice (dataPtr, dataPtr + w * h * 4);
    Module._web_free_string (dataPtr);
    Module._free (wPtr); Module._free (hPtr); Module._free (textPtr);

    /* BGRA -> RGBA in place. */
    for (let i = 0; i < bgra.length; i += 4) {
      const b = bgra[i], r = bgra[i + 2];
      bgra[i] = r; bgra[i + 2] = b;
    }
    canvas.width = w;
    canvas.height = h;
    const imageData = new ImageData (new Uint8ClampedArray (bgra.buffer), w, h);
    ctx.putImageData (imageData, 0, 0);
  }

  textInput.addEventListener ("input", render);
  sizeInput.addEventListener ("input", render);
  render ();
}) ();
