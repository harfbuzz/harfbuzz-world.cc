const { test, expect } = require ("@playwright/test");
const fs = require ("node:fs");
const path = require ("node:path");

const root = path.resolve (__dirname, "../..");
const font = fs.readFileSync (path.join (root, "fonts/NotoSans.ttf"));
const invalidFont = /not a font format supported/;
const pageErrors = new WeakMap ();

test.beforeEach (async ({ page }) => {
  // Keep font CDN, highlighting, and external demos out of these checks.
  // Tests below supply fixtures for the Google Fonts and GPU integrations.
  await page.route (/^https:\/\//, route => route.fulfill ({ body: "" }));
  const errors = [];
  pageErrors.set (page, errors);
  page.on ("pageerror", error => errors.push (error.message));
});
test.afterEach (async ({ page }) => expect (pageErrors.get (page)).toEqual ([]));

async function open (page, preset = "english", tab = "shape") {
  await page.goto (`/?preset=${preset}&theme=light#${tab}`);
  await expect (page.locator ("body")).toHaveAttribute ("data-active", tab);
  await expect (page.locator ("#hb-version")).not.toBeEmpty ();
  if (tab === "shape") await expect (page.locator ("#shape-render svg")).toBeVisible ();
}

async function picker (page) {
  if (!await page.locator ("#font-menu").isVisible ())
    await page.locator ("#font-button").click ();
}

async function fontState (page) {
  return page.evaluate (() => ({
    url: location.href,
    name: document.getElementById ("font-name").textContent,
    render: document.getElementById ("shape-render").innerHTML,
  }));
}

test ("all presets have working bundled font entries", async ({ page, request }) => {
  await open (page);
  const fonts = await page.locator ("#font-shipped option").evaluateAll (
    options => options.map (option => option.value));
  const presetFonts = await page.evaluate (() => Object.values (PRESETS).map (p => p.font));
  expect ([...fonts].sort ()).toEqual ([...presetFonts].sort ());
  for (const url of fonts) {
    const response = await request.get (url);
    expect (response.ok (), url).toBe (true);
    expect ((await response.body ()).length).toBeGreaterThan (0);
    await picker (page);
    await page.locator ("#font-shipped").selectOption (url);
    await expect.poll (() => new URL (page.url ()).searchParams.get ("font")).toBe (url);
    await expect (page.locator ("#shape-render svg")).toBeVisible ();
  }
});

test ("local tabs render and produce font, PNG, SVG, and PDF downloads", async ({ page }) => {
  await open (page);
  await expect (page.locator ("#shape-glyphs tbody tr").first ()).toBeVisible ();
  for (const tab of ["embed", "subset", "raster", "vector"]) {
    await page.locator (`.tab[data-demo="${tab}"]`).click ();
    await expect (page.locator (`#demo-${tab}`)).toBeVisible ();
  }
  const files = await page.evaluate (async () => {
    const results = {};
    for (const id of ["subset-download", "raster-dl-png", "vector-dl-svg", "vector-dl-pdf"]) {
      const link = document.getElementById (id);
      const bytes = new Uint8Array (await (await fetch (link.href)).arrayBuffer ());
      results[id] = { size: bytes.length, head: Array.from (bytes.slice (0, 8)),
        text: new TextDecoder ().decode (bytes), name: link.download };
    }
    const canvas = document.getElementById ("raster-canvas");
    results.painted = canvas.getContext ("2d").getImageData (0, 0, canvas.width, canvas.height)
      .data.some ((byte, i) => i % 4 === 3 && byte > 0);
    return results;
  });
  expect (files.painted).toBe (true);
  expect (files["subset-download"].head.slice (0, 4)).toEqual ([0, 1, 0, 0]);
  expect (files["raster-dl-png"].head).toEqual ([137, 80, 78, 71, 13, 10, 26, 10]);
  expect (files["vector-dl-svg"].text).toContain ("<svg");
  expect (files["vector-dl-pdf"].text).toMatch (/^%PDF-/);
  for (const id of ["subset-download", "raster-dl-png", "vector-dl-svg", "vector-dl-pdf"]) {
    expect (files[id].size).toBeGreaterThan (100);
    expect (files[id].name).not.toBe ("");
  }
  await expect (page.locator ("#vector-render svg")).toBeVisible ();
  await page.locator ('.tab[data-demo="subset"]').click ();
  await expect (page.locator ("#subset-preview")).toHaveText ("hello-world!");
  expect (await page.evaluate (async () => {
    const preview = document.getElementById ("subset-preview");
    const style = getComputedStyle (preview);
    return (await document.fonts.load (`${style.fontSize} ${style.fontFamily}`)).length;
  })).toBeGreaterThan (0);
});

test ("GPU tab passes the font and text to its embedded demo", async ({ page }) => {
  await page.route ("https://harfbuzz.github.io/hb-gpu-demo/**", route => route.fulfill ({
    contentType: "text/html",
    body: `<body><script>
      window.addEventListener('message', e => {
        if (e.data.kind === 'font') document.body.dataset.fontBytes = e.data.bytes.byteLength;
        if (e.data.kind === 'text') document.body.dataset.text = e.data.value;
      });
      parent.postMessage({kind:'ready'}, '*');
    </script></body>`,
  }));
  await open (page);
  await page.locator ('.tab[data-demo="gpu"]').click ();
  const frame = page.frameLocator ("#gpu-frame").locator ("body");
  await expect (frame).toHaveAttribute ("data-font-bytes", String (font.length));
  await expect (frame).toHaveAttribute ("data-text", "hello-world!");
  await page.locator ("#text").fill ("Updated text");
  await expect (frame).toHaveAttribute ("data-text", "Updated text");
});

test ("invalid URL responses preserve the current font and allow retry", async ({ page }) => {
  let body = Buffer.from ("<html>Not a font</html>");
  await page.route ("**/font-test.ttf", route => route.fulfill ({ body }));
  await open (page);
  const before = await fontState (page);
  await picker (page);
  await page.locator ("#font-url").fill ("/font-test.ttf");
  for (const bytes of [body, Buffer.alloc (0), font.subarray (0, 8)]) {
    body = bytes;
    await page.locator ("#font-url-load").click ();
    await expect (page.locator ("#font-url-error")).toHaveText (invalidFont);
    await expect (page.locator ("#font-menu")).toBeVisible ();
    expect (await fontState (page)).toEqual (before);
  }
  body = font;
  await page.locator ("#font-url-load").click ();
  await expect (page.locator ("#font-menu")).toBeHidden ();
  await expect (page.locator ("#font-url-error")).toBeEmpty ();
  expect (new URL (page.url ()).searchParams.get ("font")).toBe ("/font-test.ttf");
});

test ("invalid uploads and drops preserve the font; valid TTF, OTF, and TTC load", async ({ page }) => {
  await open (page);
  const before = await fontState (page);
  await picker (page);
  await page.locator ("#font-input").setInputFiles ({ name: "bad.ttf",
    mimeType: "font/ttf", buffer: Buffer.from ("Not a font") });
  await expect (page.locator ("#font-file-error")).toHaveText (invalidFont);
  expect (await fontState (page)).toEqual (before);
  await page.evaluate (() => {
    const transfer = new DataTransfer ();
    transfer.items.add (new File (["Not a font"], "dropped.otf"));
    document.dispatchEvent (new DragEvent ("drop", { dataTransfer: transfer, bubbles: true }));
  });
  await expect (page.locator ("#font-file-error")).toHaveText (invalidFont);
  expect (await fontState (page)).toEqual (before);

  // Wrap the bundled TTF in a one-face collection, adjusting table offsets.
  const ttc = Buffer.alloc (16 + font.length);
  ttc.write ("ttcf");
  ttc.writeUInt32BE (0x10000, 4);
  ttc.writeUInt32BE (1, 8);
  ttc.writeUInt32BE (16, 12);
  font.copy (ttc, 16);
  for (let i = 0; i < font.readUInt16BE (4); i++) {
    const offset = 12 + i * 16 + 8;
    ttc.writeUInt32BE (font.readUInt32BE (offset) + 16, 16 + offset);
  }
  for (const [name, buffer] of [["valid.ttf", font], ["valid.otf",
    fs.readFileSync (path.join (root, "fonts/NotoSansCJKsc-subset.otf"))], ["valid.ttc", ttc]]) {
    await picker (page);
    await page.locator ("#font-input").setInputFiles ({ name, mimeType: "application/octet-stream", buffer });
    await expect (page.locator ("#font-menu")).toBeHidden ();
    await expect (page.locator ("#font-file-error")).toBeEmpty ();
    expect (new URL (page.url ()).searchParams.get ("font")).toMatch (/^@/);
  }
});

test ("Google Fonts rejects non-font downloads and recovers on retry", async ({ page }) => {
  const base = "https://raw.githubusercontent.com/google/fonts/main/";
  await page.route (base + ".ci/family_features.json", route => route.fulfill ({
    json: { families: { "Test Family": { fp: "test.ttf" } } },
  }));
  let body = Buffer.from ("Not a font");
  await page.route (base + "test.ttf", route => route.fulfill ({ body }));
  await open (page);
  const before = await fontState (page);
  await picker (page);
  await page.locator ("#font-gf").fill ("Missing Family");
  await page.locator ("#font-gf-load").click ();
  await expect (page.locator ("#font-gf-error")).toContainText ("family was not found");
  await page.locator ("#font-gf").fill ("Test Family");
  await page.locator ("#font-gf-load").click ();
  await expect (page.locator ("#font-gf-error")).toHaveText (invalidFont);
  expect (await fontState (page)).toEqual (before);
  body = font;
  await page.locator ("#font-gf-load").click ();
  await expect (page.locator ("#font-menu")).toBeHidden ();
  expect (new URL (page.url ()).searchParams.get ("font")).toBe (base + "test.ttf");
});

test ("full-font errors persist, invalid responses are not cached, and retries work", async ({ page }) => {
  let response = { status: 503, body: "Unavailable" };
  await page.route ("https://raw.githubusercontent.com/googlefonts/noto-emoji/**", route => route.fulfill (response));
  await open (page, "emoji", "subset");
  const button = page.locator ("#font-full-load");
  const before = await fontState (page);
  for (const failure of [response, { status: 200, body: "<html>Not a font</html>" }]) {
    response = failure;
    await button.click ();
    await expect (button).toHaveText ("Download failed — retry");
    await expect (button).toBeEnabled ();
    await page.locator ('.tab[data-demo="embed"]').click ();
    await page.locator ('.tab[data-demo="subset"]').click ();
    await expect (button).toHaveText ("Download failed — retry");
    expect (await fontState (page)).toEqual (before);
    expect (await page.evaluate (async () => {
      const db = await fontDbOpen ();
      return new Promise (resolve => {
        const req = db.transaction (FULL_FONT_STORE).objectStore (FULL_FONT_STORE).count ();
        req.onsuccess = () => { db.close (); resolve (req.result); };
      });
    })).toBe (0);
  }
  response = { status: 200, body: font };
  await button.click ();
  await expect (page.locator ("#font-name")).toHaveText ("Noto Sans");
  await expect (button).toBeHidden ();
  await page.reload ();
  await expect (page.locator ("#font-name")).toHaveText ("Noto Sans");
  await expect (button).toBeHidden ();
});
