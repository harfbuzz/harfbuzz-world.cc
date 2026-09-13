const { test, expect } = require ("@playwright/test");
const { execFileSync } = require ("node:child_process");
const { createHash } = require ("node:crypto");
const fs = require ("node:fs");
const http = require ("node:http");
const os = require ("node:os");
const path = require ("node:path");

const root = path.resolve (__dirname, "../..");

test ("a cached build loads updated JS, CSS, and wasm from a subdirectory", async ({ page }) => {
  const temporary = fs.mkdtempSync (path.join (os.tmpdir (), "hb-world-cache-"));
  const source = path.join (temporary, "source");
  const output = path.join (temporary, "site");
  fs.mkdirSync (source);
  for (const directory of ["js", "css", "fonts"])
    fs.cpSync (path.join (root, directory), path.join (source, directory), { recursive: true });
  for (const name of fs.readdirSync (root).filter (name =>
    /\.(png|svg|ico)$/.test (name) || ["hb-world.js", "hb-world.wasm", "CNAME"].includes (name)))
    fs.copyFileSync (path.join (root, name), path.join (source, name));

  // Playwright routing disables the browser cache. Remove optional external
  // styles/scripts from this fixture instead, so this tests a real HTTP cache.
  const html = fs.readFileSync (path.join (root, "index.html"), "utf8").replace (
    /<(?:script|link)\b[^>]*(?:src|href)="https:[^"]*"[^>]*>(?:<\/script>)?/g, "");
  fs.writeFileSync (path.join (source, "index.html"), html);
  const app = fs.readFileSync (path.join (source, "js/app.js"), "utf8");
  const css = fs.readFileSync (path.join (source, "css/site.css"), "utf8");
  const wasm = fs.readFileSync (path.join (source, "hb-world.wasm"));

  function build (version) {
    fs.writeFileSync (path.join (source, "js/app.js"), app + `\nwindow.cacheTestBuild = "${version}";\n`);
    fs.writeFileSync (path.join (source, "css/site.css"), css + `\n:root { --cache-test-build: ${version}; }\n`);
    // A harmless custom section changes the wasm bytes without changing its API.
    const name = Buffer.from ("cache-test");
    const section = Buffer.concat ([Buffer.from ([name.length]), name, Buffer.from (version)]);
    fs.writeFileSync (path.join (source, "hb-world.wasm"),
      Buffer.concat ([wasm, Buffer.from ([0, section.length]), section]));
    execFileSync ("python3", [path.join (root, "scripts/package-site.py"),
      "--source", source, "--output", output]);
    const published = fs.readFileSync (path.join (output, "index.html"), "utf8");
    const assets = {};
    for (const match of published.matchAll (/(?:src|href|data-wasm)="([^"]+\.(?:js|css|wasm))"/g)) {
      const url = match[1];
      const hash = createHash ("sha256").update (fs.readFileSync (path.join (output, url))).digest ("hex").slice (0, 16);
      expect (url).toContain (`.${hash}.`);
      assets[url.replace (/\.[0-9a-f]{16}(?=\.[^.]+$)/, "")] = url;
    }
    expect (Object.keys (assets)).toHaveLength (6);
    expect (fs.readFileSync (path.join (source, "index.html"), "utf8")).toBe (html);
    expect (fs.readFileSync (path.join (output, "CNAME"), "utf8"))
      .toBe (fs.readFileSync (path.join (root, "CNAME"), "utf8"));
    return { assets, html: published };
  }

  const requests = [];
  const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css",
    ".wasm": "application/wasm", ".ttf": "font/ttf", ".otf": "font/otf", ".png": "image/png" };
  const server = http.createServer ((req, res) => {
    const pathname = new URL (req.url, "http://localhost").pathname;
    requests.push (pathname);
    const relative = pathname.slice ("/preview/".length) || "index.html";
    const file = path.resolve (output, relative);
    if (!pathname.startsWith ("/preview/") || !file.startsWith (output + path.sep) || !fs.existsSync (file)) {
      res.writeHead (404).end ();
      return;
    }
    res.writeHead (200, {
      "Content-Type": types[path.extname (file)] || "application/octet-stream",
      "Cache-Control": relative === "index.html" ? "no-cache" : "public, max-age=31536000, immutable",
    });
    res.end (fs.readFileSync (file));
  });
  const errors = [];
  page.on ("pageerror", error => errors.push (error.message));
  try {
    const first = build ("first");
    expect (build ("first")).toEqual (first); // Rebuilding unchanged content is stable.
    await new Promise (resolve => server.listen (0, "127.0.0.1", resolve));
    const url = `http://127.0.0.1:${server.address ().port}/preview/?preset=english#shape`;

    async function visit (version, assets) {
      await page.goto (url);
      await expect (page.locator ("#shape-render svg")).toBeVisible ();
      await expect.poll (() => page.evaluate (() => window.cacheTestBuild)).toBe (version);
      expect (await page.evaluate (() => getComputedStyle (document.documentElement)
        .getPropertyValue ("--cache-test-build").trim ())).toBe (version);
      const wasmUrls = await page.evaluate (() => performance.getEntriesByType ("resource")
        .filter (resource => resource.name.endsWith (".wasm")).map (resource => resource.name));
      expect (wasmUrls).toEqual ([new URL (assets["hb-world.wasm"], url).href]);
    }

    await visit ("first", first.assets);
    const second = build ("second");
    for (const name of ["js/app.js", "css/site.css", "hb-world.wasm"])
      expect (second.assets[name]).not.toBe (first.assets[name]);
    for (const name of ["hb-world.js", "js/presets.js", "js/hb-docs.js"])
      expect (second.assets[name]).toBe (first.assets[name]);
    await page.goto ("about:blank");
    await visit ("second", second.assets);

    // The unchanged loader and presets really came from cache on the second
    // visit; both versions of the changed assets were fetched exactly once.
    for (const name of ["hb-world.js", "js/presets.js", "js/hb-docs.js"])
      expect (requests.filter (url => url === "/preview/" + first.assets[name])).toHaveLength (1);
    for (const build of [first, second])
      for (const name of ["js/app.js", "css/site.css", "hb-world.wasm"])
        expect (requests.filter (url => url === "/preview/" + build.assets[name])).toHaveLength (1);
    expect (errors).toEqual ([]);
  } finally {
    if (server.listening) await new Promise (resolve => server.close (resolve));
    fs.rmSync (temporary, { recursive: true, force: true });
  }
});
