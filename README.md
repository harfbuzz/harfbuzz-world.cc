# harfbuzz-world.cc

Source for **<https://harfbuzz-world.cc>** — a live, in-browser
playground for [HarfBuzz](https://github.com/harfbuzz/harfbuzz),
plus a working example of using `src/harfbuzz-world.cc` (the
single-file HarfBuzz amalgamation) in a real project.

## Tabs

- **embed** — build HarfBuzz as a single C++ translation
  unit for use in C or C++ projects, with configuration
  flags explained.
- **shape** — inspect the glyph stream in a table alongside
  an SVG preview.
- **subset** — create and download a font subset for the
  current text, with a preview rendered using the subset.
- **raster** — render text to pixels with `hb-raster` and
  download a PNG.
- **vector** — export shaped text as SVG or PDF with
  `hb-vector`.
- **gpu** — render text using the Slug algorithm, with
  shaders for DirectX, Metal, OpenGL, OpenGL ES, WebGL2,
  and WebGPU. This site's WebGL2 demo is embedded from
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

Produces `hb-world.js` + `hb-world.wasm` at the repo root.
Then:

```sh
python3 -m http.server -d .
```

…and visit <http://localhost:8000/>.

## Deploy

GitHub Actions (`.github/workflows/pages.yml`) builds the
WebAssembly bundle on every push to `main` and publishes via
`actions/deploy-pages` to <https://harfbuzz-world.cc>.

## Repository layout

```
src/
  bindings.cc          Emscripten C exports (web_render_*, web_subset, ...)
  config.h             HB_TINY base + HB_HAS_RASTER/VECTOR/SUBSET
  config-override.h    things HB_TINY disables that we need back

scripts/
  build.sh             em++ invocation
fonts/                 bundled OFL font subsets
js/app.js              SPA shell + per-demo render code
css/site.css           styles
index.html             single page, all tabs
.github/workflows/     CI
```

## License

The site code is under the same MIT license as HarfBuzz.
Bundled fonts are OFL ([Noto](https://notofonts.github.io/)).
