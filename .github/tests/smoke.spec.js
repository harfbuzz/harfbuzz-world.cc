const { test, expect } = require ("@playwright/test");
const fs = require ("node:fs");
const path = require ("node:path");

const root = path.resolve (__dirname, "../..");
const font = fs.readFileSync (path.join (root, "fonts/NotoSans.ttf"));
const invalidFont = "This demo does not support this font format.";
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

function collection (fonts) {
  let next = 12 + 4 * fonts.length;
  const offsets = fonts.map (bytes => { const offset = next; next += (bytes.length + 3) & ~3; return offset; });
  const ttc = Buffer.alloc (next);
  ttc.write ("ttcf");
  ttc.writeUInt32BE (0x10000, 4);
  ttc.writeUInt32BE (fonts.length, 8);
  fonts.forEach ((bytes, index) => {
    const base = offsets[index];
    ttc.writeUInt32BE (base, 12 + index * 4);
    bytes.copy (ttc, base);
    for (let i = 0; i < bytes.readUInt16BE (4); i++) {
      const offset = 12 + i * 16 + 8;
      ttc.writeUInt32BE (bytes.readUInt32BE (offset) + base, base + offset);
    }
  });
  return ttc;
}

test ("a visit without a tab starts on Embed before wasm loads", async ({ page }) => {
  let release;
  const pending = new Promise (resolve => { release = resolve; });
  await page.route ("**/*.wasm", async route => {
    await pending;
    await route.continue ();
  });
  try {
    await page.goto ("/", { waitUntil: "domcontentloaded" });
    await expect (page.locator ("#hb-version")).toBeEmpty ();
    await expect (page.locator ("#demo-embed")).toBeVisible ();
    await expect (page.locator ('.tab[data-demo="embed"]')).toHaveClass (/active/);
    for (const selector of ["#shared-controls", ".controls-row2", ".presets", ".render"])
      await expect (page.locator (selector + ":visible")).toHaveCount (0);
    const before = await page.locator ("#demo-embed").boundingBox ();
    release ();
    await expect (page.locator ("#hb-version")).not.toBeEmpty ();
    expect (await page.locator ("#demo-embed").boundingBox ()).toEqual (before);
    await expect (page.locator ("#shared-controls")).toBeHidden ();
  } finally {
    release ();
  }
});

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
  await expect (page.locator (".tabs .tab")).toHaveText (["embed", "shape", "subset", "raster", "vector", "gpu", "info"]);
  await expect (page.locator (".demo-links a")).toHaveText (["shape", "subset", "raster", "vector", "gpu", "info"]);
  await expect (page.locator ("#shape-glyphs tbody tr").first ()).toBeVisible ();
  for (const tab of ["embed", "subset", "raster", "vector", "info"]) {
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
  await page.locator ('.tab[data-demo="vector"]').click ();
  await expect (page.locator ("#vector-render svg")).toBeVisible ();
  await page.locator ('.tab[data-demo="subset"]').click ();
  await expect (page.locator ("#subset-preview")).toHaveText ("hello-world!");
  expect (await page.evaluate (async () => {
    const preview = document.getElementById ("subset-preview");
    const style = getComputedStyle (preview);
    return (await document.fonts.load (`${style.fontSize} ${style.fontFamily}`)).length;
  })).toBeGreaterThan (0);
});

test ("code snippets follow shaping controls across tabs and copy as plain C", async ({ page }) => {
  await page.addInitScript (() => {
    Object.defineProperty (navigator, "clipboard", {
      value: { writeText: async text => { window.copiedSnippet = text; } },
    });
  });
  await page.route ("**/snippet.ttc", route => route.fulfill ({ body: collection ([font, font]) }));
  await page.goto ("/?font=/snippet.ttc&face=1&text=office&size=43.35"
    + "&variations=wght=625.35,wdth=83.25&features=liga=0,kern=1&cluster-level=1&theme=dark&open=code#shape");
  await expect (page.locator ("#shape-glyphs tbody tr")).toHaveCount (6);
  for (const tab of ["shape", "raster", "vector", "gpu", "info"]) {
    await page.locator (`.tab[data-demo="${tab}"]`).click ();
    const snippet = page.locator (`#${tab}-snippet`);
    await expect (snippet).toContainText ("hb_face_create (blob, 1)");
    await expect (snippet).toContainText ("hb_font_set_variations (font, variations,");
    await expect (snippet).toContainText ("{ HB_TAG ('w', 'g', 'h', 't'), 625.35f }");
    await expect (snippet).toContainText ("{ HB_TAG ('w', 'd', 't', 'h'), 83.25f }");
    if (tab !== "info") {
      await expect (snippet).toContainText ('hb_feature_from_string ("liga=0", -1, &features[0])');
      await expect (snippet).toContainText ('hb_feature_from_string ("kern=1", -1, &features[1])');
      await expect (snippet).toContainText ("hb_shape (font, buf, features, 2)");
      await expect (snippet).toContainText ('hb_buffer_add_utf8 (buf, "office", -1, 0, -1)');
    }
    if (["shape", "raster", "vector"].includes (tab)) {
      await expect (snippet).toContainText ("#define FONT_SIZE_PX  43.35f");
      await expect (snippet).toContainText ("hb_font_set_scale (font, FONT_SIZE_PX * SCALE, FONT_SIZE_PX * SCALE)");
      await expect (snippet).toContainText ("HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS");
    } else {
      await expect (snippet).not.toContainText ("FONT_SIZE_PX");
      await expect (snippet).not.toContainText ("hb_buffer_set_cluster_level");
    }
    if (["raster", "vector", "gpu"].includes (tab)) {
      await expect (snippet).toContainText (`hb_${tab}_paint_set_palette (p, 0)`);
      await expect (snippet).toContainText ("HB_COLOR (255, 255, 255, 255)");
      await expect (snippet).toContainText ("HB_COLOR (34, 34, 34, 255)");
    }
  }
  await page.locator ('.tab[data-demo="shape"]').click ();
  const text = '"\\{shape} hb_shape <&>\u0001' + '7';
  await page.locator ("#text").fill (text);
  const snippet = page.locator ("#shape-snippet");
  await expect (snippet).toContainText ('hb_buffer_add_utf8 (buf, "\\"\\\\{shape} hb_shape <&>\\0017", -1, 0, -1)');
  await page.locator ('[data-snippet="shape"] .snippet-copy').click ();
  expect (await page.evaluate (() => window.copiedSnippet)).toBe (await snippet.textContent ());

  await page.locator ("#feat-button").click ();
  await page.locator ("#feat-reset").click ();
  await page.locator ("#feat-button").click ();
  await page.locator ("#var-button").click ();
  await page.locator ("#var-reset").click ();
  await page.locator ("#var-button").click ();
  await page.locator ("#shape-cluster-level").selectOption ("0");
  await expect (snippet).toContainText ("hb_shape (font, buf, NULL, 0)");
  await expect (snippet).not.toContainText ("hb_feature_from_string");
  await expect (snippet).not.toContainText ("hb_font_set_variations");
  await expect (snippet).not.toContainText ("hb_buffer_set_cluster_level");
});

test ("subset snippets reflect axis pinning and layout feature retention", async ({ page }) => {
  await page.goto ("/?preset=english&text=office&variations=wght=625.35"
    + "&features=liga=0,kern=1&open=code#subset");
  const snippet = page.locator ("#subset-snippet");
  await expect (page.locator ("#subset-download")).toHaveAttribute ("href", /^blob:/);
  await expect (snippet).toContainText ("hb_subset_input_pin_axis_location (input, face, HB_TAG ('w', 'g', 'h', 't'), 625.35f)");
  await expect (snippet).toContainText ("hb_subset_input_pin_axis_location (input, face, HB_TAG ('w', 'd', 't', 'h'), 100.0f)");
  await expect (snippet).toContainText ("hb_set_del (layout_features, HB_TAG ('l', 'i', 'g', 'a'))");
  await expect (snippet).toContainText ("hb_set_add (layout_features, HB_TAG ('k', 'e', 'r', 'n'))");
  await expect (snippet).toContainText ('hb_buffer_add_utf8 (buf, "office", -1, 0, -1)');
  await expect (snippet).not.toContainText ("hb_shape (");

  await page.locator ("#subset-instantiate").uncheck ();
  await expect (snippet).not.toContainText ("hb_subset_input_pin_axis_location");
  await expect (snippet).not.toContainText ("625.35");
  await expect (snippet).toContainText ("hb_set_del (layout_features,");
  await page.locator ("#subset-instantiate").check ();
  await expect (snippet).toContainText ("625.35f");
});

test ("Info reports hb-info categories and lazily renders glyph SVG grids", async ({ page }) => {
  await open (page, "english", "info");
  await expect (page.locator ("#info-summary")).toContainText ("Noto Sans");
  await expect (page.locator ("#info-summary")).toContainText ("TrueType outlines");
  await expect (page.locator ("#info-names-count")).not.toBeEmpty ();
  await expect (page.locator ("#info-tables-count")).not.toBeEmpty ();
  await expect (page.locator (".controls-row2")).toBeHidden ();
  await expect (page.locator ("#feat-picker")).toBeHidden ();
  await expect (page.locator ("#var-picker")).toBeHidden ();
  await expect (page.locator ('#demo-info [data-section="palettes"]')).toBeHidden ();
  await expect (page.locator ('#demo-info [data-section="meta"]')).toBeHidden ();
  await expect (page.locator ('#demo-info [data-section="variations"]')).toBeVisible ();

  await expect (page.locator ("#info-characters .info-glyph-card")).toHaveCount (0);
  await page.locator ("#info-characters-wrap > summary").click ();
  await expect (page.locator ("#info-characters .info-glyph-card")).toHaveCount (48);
  await expect (page.locator ("#info-characters .info-glyph-art svg").first ()).toBeVisible ();
  await expect (page.locator ("#info-characters .info-glyph-code").first ()).toContainText ("U+");
  await page.locator ("#info-characters").evaluate (el => { el.scrollTop = el.scrollHeight; });
  await expect (page.locator ("#info-characters .info-glyph-card")).toHaveCount (95);

  await page.locator ("#info-glyphs-wrap > summary").click ();
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (48);
  await expect (page.locator ("#info-glyphs .info-glyph-art svg").first ()).toBeVisible ();
  await expect (page.locator ("#info-glyphs .info-glyph-code").first ()).toHaveText ("gid0");
  await page.locator ("#info-glyphs").evaluate (el => { el.scrollTop = el.scrollHeight; });
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (96);
  await page.locator ("#info-glyphs").evaluate (el => { el.scrollTop = el.scrollHeight; });
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (132);
  await expect (page.locator ("#info-glyphs .info-glyph-code").last ()).toHaveText ("gid131");
  await expect (page.getByRole ("button", { name: /Show (all|more)/ })).toHaveCount (0);
  for (const id of ["info-characters", "info-glyphs"]) {
    const grid = page.locator ("#" + id);
    expect (await grid.evaluate (el => el.scrollHeight > el.clientHeight)).toBe (true);
    await grid.focus ();
    await page.keyboard.press ("End");
    await expect.poll (() => grid.evaluate (el => el.scrollTop)).toBeGreaterThan (0);
  }
});

test ("Info jumps to characters and glyph IDs in context, without filtering the grid", async ({ page }) => {
  await open (page, "english", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  const search = page.getByRole ("searchbox", { name: "Search characters" });
  for (const query of ["U+007E", "0x7e", "007e", "~"]) {
    await search.fill (query);
    await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText ("U+007E");
    await expect (page.locator ("#info-characters .info-glyph-card").filter ({ hasText: "U+007D" })).toHaveCount (1);
  }
  for (const [query, code] of [["A", "U+0041"], [" ", "U+0020"], ["0", "U+0030"]]) {
    await search.fill (query);
    await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText (code);
  }
  const beforeMiss = await page.locator ("#info-characters .info-glyph-card").count ();
  await search.fill ("U+1F600");
  await expect (page.locator ("#info-characters-status")).toHaveText ("0 matches");
  await expect (page.locator ("#info-characters-wrap")).toBeVisible ();
  await expect (page.locator ("#info-characters .info-glyph-card")).toHaveCount (beforeMiss);
  await expect (page.locator ("#info-characters .info-match")).toHaveCount (0);
  await search.fill ("");
  await expect (page.locator ("#info-characters .info-glyph-card")).toHaveCount (beforeMiss);

  await page.locator ("#info-glyphs-wrap > summary").click ();
  const glyphSearch = page.getByRole ("searchbox", { name: "Search glyphs" });
  for (const query of ["131", "gid131", "GID131"]) {
    await glyphSearch.fill (query);
    await expect (page.locator ("#info-glyphs .info-match .info-glyph-code")).toHaveText ("gid131");
    await expect (page.locator ('#info-glyphs .info-glyph-code').filter ({ hasText: /^gid130$/ })).toHaveCount (1);
    await expect.poll (() => page.locator ("#info-glyphs .info-match").evaluate (el => {
      const a = el.getBoundingClientRect (), b = el.parentElement.getBoundingClientRect ();
      return a.top >= b.top - 1 && a.bottom <= b.bottom + 1;
    })).toBe (true);
    await expect (glyphSearch).toBeFocused ();
  }
  await glyphSearch.fill ("gid1");
  await expect (page.locator ("#info-glyphs .info-match .info-glyph-code")).toHaveText ("gid1");
  await glyphSearch.fill ("no-such-glyph");
  await expect (page.locator ("#info-glyphs-status")).toHaveText ("0 matches");
  await glyphSearch.fill ("");
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (48);
  await expect (page.locator ("#info-glyphs-count")).toHaveText ("132");
});

test ("open sections round-trip in URLs across tabs and closed deep links stay closed", async ({ page }) => {
  await open (page, "english", "info");
  const names = page.locator ('#demo-info [data-section="names"]');
  const glyphs = page.locator ("#info-glyphs-wrap");
  await names.locator ("summary").click ();
  await glyphs.locator ("summary").click ();
  await expect.poll (() => new URL (page.url ()).searchParams.get ("open")).toBe ("info.names,info.glyphs");
  await page.locator ('.tab[data-demo="shape"]').click ();
  await page.locator ('#demo-shape [data-snippet] > summary').click ();
  await expect.poll (() => new URL (page.url ()).searchParams.get ("open")).toContain ("code");
  await page.reload ();
  await expect (page.locator ('#demo-shape [data-snippet]')).toHaveAttribute ("open", "");
  await page.locator ('.tab[data-demo="info"]').click ();
  await expect (names).toHaveAttribute ("open", "");
  await expect (glyphs).toHaveAttribute ("open", "");
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (48);
  await page.goto ("/?preset=english&open=#info/glyphs");
  await expect (glyphs).toHaveAttribute ("open", "");
  await glyphs.locator ("summary").click ();
  await expect.poll (() => new URL (page.url ()).hash).toBe ("#info");
  await expect.poll (() => new URL (page.url ()).searchParams.get ("open")).toBe ("");
  await page.reload ();
  await expect (page.locator ("#info-summary")).toContainText ("Noto Sans");
  await expect (glyphs).not.toHaveAttribute ("open", "");
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (0);
});

test ("collection face selection drives Info, rendering, subsetting, URLs, and the GPU font", async ({ page }) => {
  const otf = fs.readFileSync (path.join (root, "fonts/AdobeBlank.otf"));
  await page.route ("**/faces.ttc", route => route.fulfill ({ body: collection ([font, otf]) }));
  await page.route ("https://harfbuzz.github.io/hb-gpu-demo/**", route => route.fulfill ({
    contentType: "text/html",
    body: `<body><script>
      window.addEventListener('message', async e => {
        if (e.data.kind !== 'font') return;
        const bytes = new Uint8Array(e.data.bytes);
        document.body.dataset.signature = String.fromCharCode(...bytes.slice(0, 4));
        try { await new FontFace('selected', e.data.bytes).load(); document.body.dataset.valid = 'yes'; }
        catch { document.body.dataset.valid = 'no'; }
      });
      parent.postMessage({kind:'ready'}, '*');
    </script></body>`,
  }));
  await page.goto ("/?font=/faces.ttc&face=1&open=info.glyphs#info");
  await expect (page.locator ("#font-face")).toHaveValue ("1");
  await expect (page.locator ("#font-face option")).toHaveCount (2);
  await expect (page.locator ("#info-summary")).toContainText ("PostScript outlines");
  await expect (page.locator ('#demo-info [data-section="variations"]')).toBeHidden ();
  await expect (page.locator ("#info-glyphs .info-glyph-card")).toHaveCount (48);
  await page.getByRole ("searchbox", { name: "Search glyphs" }).fill ("2048");
  await expect (page.locator ("#info-glyphs .info-match .info-glyph-code")).toHaveText ("gid2048");
  expect (await page.locator ("#info-glyphs .info-glyph-card").count ()).toBeLessThan (150);
  const firstIndex = await page.locator ("#info-glyphs .info-glyph-card").first ().getAttribute ("data-index");
  await page.locator ("#info-glyphs").evaluate (el => { el.scrollTop = 0; });
  await expect.poll (async () => Number (await page.locator ("#info-glyphs .info-glyph-card").first ().getAttribute ("data-index")))
    .toBeLessThan (Number (firstIndex));
  await page.getByRole ("searchbox", { name: "Search glyphs" }).fill ("GID");
  await expect (page.locator ("#info-glyphs-status")).toHaveText ("2,049 matches");
  await page.locator ('.tab[data-demo="shape"]').click ();
  await expect (page.locator ("body")).toHaveAttribute ("data-active", "shape");
  const cffShape = await page.locator ("#shape-render").innerHTML ();
  await page.locator ("#font-face").selectOption ("0");
  await expect (page.locator ("#shape-render")).not.toHaveJSProperty ("innerHTML", cffShape);
  await page.locator ("#font-face").selectOption ("1");
  await expect (page.locator ("#shape-render")).toHaveJSProperty ("innerHTML", cffShape);
  await page.locator ('.tab[data-demo="subset"]').click ();
  await expect (page.locator ("#subset-download")).toHaveAttribute ("href", /^blob:/);
  expect (await page.evaluate (async () => {
    const bytes = new Uint8Array (await (await fetch (document.getElementById ("subset-download").href)).arrayBuffer ());
    return String.fromCharCode (...bytes.slice (0, 4));
  })).toBe ("OTTO");
  await page.locator ('.tab[data-demo="gpu"]').click ();
  const frame = page.frameLocator ("#gpu-frame").locator ("body");
  await expect (frame).toHaveAttribute ("data-signature", "OTTO");
  await expect (frame).toHaveAttribute ("data-valid", "yes");
  await page.locator ("#font-face").selectOption ("0");
  await expect (frame).not.toHaveAttribute ("data-signature", "OTTO");
  await page.locator ("#font-face").selectOption ("1");
  await expect (frame).toHaveAttribute ("data-signature", "OTTO");
  await expect.poll (() => new URL (page.url ()).searchParams.get ("face")).toBe ("1");
  await page.reload ();
  await expect (page.locator ("#font-face")).toHaveValue ("1");
  await picker (page);
  await page.locator ("#font-shipped").selectOption ("fonts/NotoSans.ttf");
  await expect (page.locator ("#font-face-label")).toBeHidden ();
  await expect.poll (() => new URL (page.url ()).searchParams.get ("face")).toBe (null);
});

test ("Info searches named glyphs and supplementary or variation-sequence characters", async ({ page }) => {
  await open (page, "hebrew", "info");
  await page.locator ("#info-glyphs-wrap > summary").click ();
  await page.getByRole ("searchbox", { name: "Search glyphs" }).fill (".NOTDEF");
  await expect (page.locator ("#info-glyphs .info-glyph-code")).toHaveText ("gid0");
  await open (page, "emoji", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  const search = page.getByRole ("searchbox", { name: "Search characters" });
  for (const query of ["🥰", "U+1F970"]) {
    await search.fill (query);
    await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText ("U+1F970");
    await expect (page.locator ("#info-characters .info-match .info-glyph-art svg")).toBeVisible ();
  }
  for (const query of ["❤️", "U+2764 U+FE0F"]) {
    await search.fill (query);
    await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText ("U+2764 U+FE0F");
  }
  await search.fill ("U+2764");
  await expect (page.locator ("#info-characters-status")).toHaveText ("1 of 2 matches");
  await search.press ("Enter");
  await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText ("U+2764 U+FE0F");
  await expect (page.locator ("#info-characters-status")).toHaveText ("2 of 2 matches");
  await search.press ("Shift+Enter");
  await expect (page.locator ("#info-characters .info-match .info-glyph-code")).toHaveText ("U+2764");
  await page.getByRole ("button", { name: "Previous character match" }).click ();
  await expect (page.locator ("#info-characters-status")).toHaveText ("2 of 2 matches");
  await page.getByRole ("button", { name: "Next character match" }).click ();
  await expect (page.locator ("#info-characters-status")).toHaveText ("1 of 2 matches");
});

test ("character-map searches resolve explicit glyph IDs and exact glyph names", async ({ page }) => {
  await open (page, "hebrew", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  const search = page.getByRole ("searchbox", { name: "Search characters" });
  const match = page.locator ("#info-characters .info-match .info-glyph-code");
  for (const [query, code] of [["name:A", "U+0041"], ["name:a", "U+0061"],
                              ["name:uni05D0", "U+05D0"], ["gid3", "U+05D0"],
                              ["GID3", "U+05D0"], ["3", "U+0033"]]) {
    await search.fill (query);
    await expect (match).toHaveText (code);
    await expect (page.locator ("#info-characters-status")).toHaveText ("1 match");
  }
  // Explicit name matching must not become a substring search, or admit
  // unencoded glyphs into the character map. Bad IDs must not select gid0.
  for (const query of ["name:", "name:uni05", "name:UNI05D0", "name:.notdef", "gid0",
                       "gid", "gid-1", "gid3x", "gid999999999999999999999999999999"]) {
    await search.fill (query);
    await expect (page.locator ("#info-characters-status")).toHaveText ("0 matches");
    await expect (match).toHaveCount (0);
    expect (await page.locator ("#info-characters .info-glyph-card").count ()).toBeGreaterThan (1);
  }
  await page.locator ("#info-glyphs-wrap > summary").click ();
  await page.getByRole ("searchbox", { name: "Search glyphs" }).fill (".notdef");
  await expect (page.locator ("#info-glyphs .info-glyph-code")).toHaveText ("gid0");

  await open (page, "emoji", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  await search.fill ("gid29");
  await expect (page.locator ("#info-characters-status")).toHaveText ("1 of 2 matches");
  await expect (match).toHaveText ("U+2764");
  await page.getByRole ("button", { name: "Next character match" }).click ();
  await expect (match).toHaveText ("U+2764 U+FE0F");
  await expect (page.locator ("#info-characters-status")).toHaveText ("2 of 2 matches");
  // Switching fonts re-resolves the query against the newly selected face.
  await picker (page);
  await page.locator ("#font-shipped").selectOption ("fonts/NotoSansHebrew.ttf");
  await search.fill ("name:A");
  await expect (match).toHaveText ("U+0041");
});

test ("Info cards show glyph IDs before real names and leave missing names blank", async ({ page }) => {
  await open (page, "emoji", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  const first = page.locator ("#info-characters .info-glyph-card").first ();
  await expect (first.locator (".info-glyph-id")).toHaveText ("gid1");
  await expect (first.locator (".info-glyph-id + .info-glyph-name")).toHaveText ("");
  expect (await first.locator (".info-glyph-name").evaluate (el => el.getBoundingClientRect ().height)).toBeGreaterThan (0);
  await expect (first).toContainText ("U+200D");
  await page.locator ("#info-glyphs-wrap > summary").click ();
  const glyph = page.locator ("#info-glyphs .info-glyph-card").first ();
  await expect (glyph.locator (".info-glyph-code")).toHaveText ("gid0");
  await expect (glyph.locator (".info-glyph-code + .info-glyph-name")).toHaveText ("");
  await expect (glyph.locator (".info-glyph-id")).toHaveCount (0);

  await open (page, "hebrew", "info");
  await page.locator ("#info-characters-wrap > summary").click ();
  await page.getByRole ("searchbox", { name: "Search characters" }).fill ("name:A");
  const named = page.locator ("#info-characters .info-match");
  await expect (named.locator (".info-glyph-code")).toHaveText ("U+0041");
  await expect (named.locator (".info-glyph-id")).toHaveText (/^gid\d+$/);
  await expect (named.locator (".info-glyph-id + .info-glyph-name")).toHaveText ("A");
  await page.locator ("#info-glyphs-wrap > summary").click ();
  await expect (page.locator ("#info-glyphs .info-glyph-card").first ().locator (".info-glyph-name")).toHaveText (".notdef");
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

  const ttc = collection ([font]);
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
