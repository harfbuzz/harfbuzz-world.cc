# harfbuzz-world.cc

Source for **<https://harfbuzz-world.cc>** — a live, in-browser
playground for [HarfBuzz](https://github.com/harfbuzz/harfbuzz)
shaping, subsetting, and rendering. The site also demonstrates
how to use `src/harfbuzz-world.cc`, a single-file amalgamation
for C and C++ projects.

## Tabs

- **embed** — compile and link `src/harfbuzz-world.cc` as a
  single C++ translation unit, with configuration flags
  explained.
- **shape** — inspect the glyph stream in a table alongside
  an SVG preview.
- **subset** — create and download a font subset for the
  current text, with a preview rendered using the subset.
- **raster** — render text to pixels with `hb-raster` and
  download a PNG.
- **vector** — export shaped text as SVG or PDF with
  `hb-vector`.
- **gpu** — render text using the Slug algorithm, with support
  for DirectX, Metal, OpenGL, OpenGL ES, WebGL2, and WebGPU.
  This site's WebGL2 demo is embedded from
  [hb-gpu-demo](https://harfbuzz.github.io/hb-gpu-demo/)
  and driven by the same shared controls.

The demos share text, font, variation, and OpenType feature
controls. Size applies to shape, subset, raster, and vector;
the GPU demo uses zoom gestures.

Nine presets cover emoji, English, Hebrew, Arabic, Urdu,
Hindi, Thai, Khmer, and Chinese. Presets pair sample text
with bundled fonts. After you load a custom font, presets
change only the text.

## Build

Prerequisites:

- [Emscripten](https://emscripten.org/) on `$PATH`
  (`source emsdk/emsdk_env.sh`).
- A HarfBuzz source tree specified by `$HB_SRC`, in
  `./harfbuzz/`, or at `$HOME/harfbuzz`.

```sh
bash scripts/build.sh
```

Produces `hb-world.js` + `hb-world.wasm` at the repo root,
then packages the publishable site in `dist/`. JavaScript,
CSS, and WebAssembly filenames include content hashes;
the generated HTML selects the matching WebAssembly file.
Unchanged resources keep the same URLs across builds.
The footer links to the HarfBuzz commit compiled into the bundle
(`-dirty` marks local changes) and shows the UTC packaging date.

Preview the packaged site with:

```sh
python3 -m http.server -d dist
```

…and visit <http://localhost:8000/>.

After editing HTML, JavaScript, or CSS, refresh the package
without recompiling WebAssembly:

```sh
python3 scripts/package-site.py
```

Serving the repository root still works for source previews,
using unversioned URLs. `dist/` is generated and not committed.

## Check

After building the WebAssembly bundle, run the browser smoke
tests with Node.js 22 or newer:

```sh
npm ci --prefix .github/tests
npm --prefix .github/tests exec -- playwright install chromium
npm --prefix .github/tests test
```

The tests refresh `dist/` and serve it on port 8003. They check
bundled fonts, tab rendering, download contents, invalid font
data, load retries, and updates with older resources cached.
Google Fonts and the GPU iframe use
local response fixtures; the GPU check covers the host's
message exchange, not the external renderer.

## Deploy

GitHub Actions (`.github/workflows/pages.yml`) builds the
WebAssembly bundle and runs the smoke tests on every push to
`main`, then uploads `dist/` and publishes via
`actions/deploy-pages` to <https://harfbuzz-world.cc>.

## Repository layout

```
src/
  bindings.cc          Emscripten C exports (web_render_*, web_subset, ...)
  config.h             HB_TINY base + HB_HAS_RASTER/VECTOR/SUBSET
  config-override.h    things HB_TINY disables that we need back

scripts/
  build.sh             em++ invocation
  package-site.py      static site packaging and resource hashes
dist/                  generated site published to GitHub Pages
fonts/                 bundled OFL font subsets
js/app.js              SPA shell + per-demo render code
css/site.css           styles
index.html             single page, all tabs
.github/workflows/     CI
```

## License

The site code is under the same MIT license as HarfBuzz.
Bundled fonts are OFL ([Noto](https://notofonts.github.io/)).
