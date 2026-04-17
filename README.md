# harfbuzz-world.cc

Source for **<https://harfbuzz-world.cc>** — a live, in-browser
playground for [HarfBuzz](https://github.com/harfbuzz/harfbuzz),
plus a working example of using `src/harfbuzz-world.cc` (the
single-file HarfBuzz amalgamation) in a real project.

## Tabs

- **embed** — how to drop `harfbuzz-world.cc` into your own
  C/C++ build, with configuration flags explained.
- **shape** — text → glyph stream as JSON, with an SVG
  preview of the laid-out result.
- **subset** — subsets the current font for the current
  text via `hb-subset`; downloadable, with a live preview
  rendered using the subset itself.
- **raster** — pixel-perfect software rendering through
  `hb-raster`, blitted straight into `<canvas>`.
- **vector** — SVG and PDF output via `hb-vector`, both
  downloadable; SVG also rendered inline as a preview.
- **gpu** — slug-based GPU rendering, embedded from
  [hb-gpu-demo](https://harfbuzz.github.io/hb-gpu-demo/)
  via iframe and driven by the same shared controls.

The shared controls (text, size, font picker, variable-axis
sliders) feed every tab.  Five presets (Latin / Arabic /
Devanagari / Chinese / Emoji) load matching script + font
combos with one click.

## Build

Prerequisites:

- [Emscripten](https://emscripten.org/) on `$PATH`
  (`source emsdk/emsdk_env.sh`).
- A HarfBuzz source tree, either at `$HB_SRC`, in a sibling
  `harfbuzz/` directory, or `$HOME/harfbuzz`.

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
wasm bundle on every push to `main` and publishes via
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
