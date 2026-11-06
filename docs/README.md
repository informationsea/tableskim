# TableSkim Documentation

- [English](en/README.md)
- [日本語](ja/README.md)

Screenshots shared by all languages are in [images/](images/).

The walkthroughs use the public datasets in `demo/examples/`; sources and
licenses are recorded in `demo/examples/README.md`. The localized
`examples.md` pages explain units, missing values and the steps to reproduce
the figures. Format-specific examples (such as Excel sheets) also use
`demo/data/`.

To refresh the screenshots from the real interfaces, run from the repository
root after building the native and web apps:

```sh
TABLESKIM_SCREENSHOT_DIR=docs/images xvfb-run -a -s "-screen 0 1600x1200x24" \
    build/debug/tableskim-gui-tests "[.screenshots]"
python3 tests/tui/screenshots.py --tableskim build/debug/tableskim
LANG=C.UTF-8 node web/screenshots.cjs build/wasm-release
```

The GUI command needs Xvfb on Linux; the TUI script needs pyte and Pillow;
the web script needs the packages installed in `web/` and Playwright's
Chromium (or `TABLESKIM_CHROMIUM` pointing to a local Chromium). SQL screenshots
convert a temporary copy of Iris to Parquet; the public datasets are not
modified. `gui-multisheet.png` is the format-specific screenshot taken by
hand. Inspect the generated images before committing them.
