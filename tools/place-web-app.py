#!/usr/bin/env python3
"""Puts the web app on the built site, from a release's web ZIP.

    tools/place-web-app.py <site directory> [tableskim-<version>-web.zip]

The ZIP (cmake/WebZip.cmake: one folder holding the app, its licenses and a
README) is unpacked into app/ of the site, and the links to it, the
elements marked data-web-app, are shown: they are hidden in the pages, so
that a site built without the app (by hand, or for a release without the
ZIP) links to nothing missing. The app needs no server support: on GitHub
Pages, its coi-serviceworker.js makes the page cross-origin isolated.
"""

import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

PAGES = ["index.html", "ja/index.html", "download.html", "ja/download.html"]
MARK = "data-web-app hidden"


def unpack(archive, target):
    with tempfile.TemporaryDirectory() as work, zipfile.ZipFile(archive) as zip_file:
        # zipfile keeps every member inside the directory it extracts to.
        zip_file.extractall(work)
        folders = [entry for entry in Path(work).iterdir() if entry.is_dir()]
        if len(folders) != 1 or not (folders[0] / "index.html").is_file():
            raise SystemExit(f"place-web-app: {archive} does not hold one folder with the app")
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(folders[0], target)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    site = Path(sys.argv[1])
    if len(sys.argv) == 2:
        print("place-web-app: no web app; its links stay hidden")
        return
    unpack(Path(sys.argv[2]), site / "app")
    for page in PAGES:
        path = site / page
        text = path.read_text(encoding="utf-8")
        if MARK not in text:
            raise SystemExit(f"place-web-app: {page} has no link to the web app")
        path.write_text(text.replace(MARK, "data-web-app"), encoding="utf-8")
    print(f"Web app placed in {site / 'app'}")


if __name__ == "__main__":
    main()
