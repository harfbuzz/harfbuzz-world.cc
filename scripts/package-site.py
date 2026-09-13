#!/usr/bin/env python3
"""Package the static site with content-addressed JS, CSS, and WebAssembly."""

import argparse
import hashlib
from pathlib import Path
import re
import shutil


def package_site(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output:
        raise ValueError("The output directory must differ from the source directory")

    assets = [source / "hb-world.js", source / "hb-world.wasm"]
    for directory, suffix in [("js", ".js"), ("css", ".css")]:
        assets.extend(sorted((source / directory).rglob("*" + suffix)))

    # Hash the bytes we publish, independently of timestamps and Git state.
    # Read everything before writing so a missing wasm cannot replace the page.
    contents = {asset.relative_to(source).as_posix(): asset.read_bytes() for asset in assets}
    urls = {}
    for name, data in contents.items():
        path = Path(name)
        digest = hashlib.sha256(data).hexdigest()[:16]
        urls[name] = path.with_name(f"{path.stem}.{digest}{path.suffix}").as_posix()

    def rewrite(match):
        return match[1] + urls.get(match[2], match[2]) + match[3]

    html = re.sub(r'(\b(?:src|href|data-wasm)=")([^"]+)(")', rewrite,
                  (source / "index.html").read_text())

    output.mkdir(parents=True, exist_ok=True)
    for name, data in contents.items():
        target = output / urls[name]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    # Font URLs are also used in share links and the font picker. Keep those
    # stable, along with the images used by the tab switcher and social cards.
    shutil.copytree(source / "fonts", output / "fonts", dirs_exist_ok=True)
    for pattern in ("*.png", "*.svg", "*.ico", "CNAME"):
        for asset in source.glob(pattern):
            shutil.copy2(asset, output / asset.name)

    # Publish the entry point last. Leave older hashed files available during
    # local rebuilds for pages that are still using the previous entry point.
    temporary = output / "index.html.tmp"
    temporary.write_text(html)
    temporary.replace(output / "index.html")
    return urls


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.source / "dist"
    urls = package_site(args.source, output)
    print(f"Packaged {output} ({len(urls)} versioned assets)")
