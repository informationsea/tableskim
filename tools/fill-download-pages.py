#!/usr/bin/env python3
"""Fills the download pages of a built site in from a GitHub release.

    tools/fill-download-pages.py <release.json> <site directory>

<release.json> is the GitHub API's answer for the release
(gh api repos/<owner>/<repo>/releases/latest). Each file card
(<article class="file" data-asset="...">) gets the link, name and size of
the asset its pattern matches, or the class "missing" when the release has
none; the release's version, date and notes are shown at the top. Pages
left unfilled still work: every button opens the latest release.
"""

import datetime
import html
import json
import re
import sys
from pathlib import Path

# Asset names as cmake/Packaging.cmake makes them.
PATTERNS = {
    "windows-installer": r"tableskim-[\d.]+-Windows-[^-]+\.exe",
    "windows-zip": r"tableskim-[\d.]+-Windows-[^-]+\.zip",
    "macos-dmg": r"tableskim-[\d.]+-Darwin-arm64\.dmg",
    "macos-zip": r"tableskim-[\d.]+-Darwin-arm64\.zip",
    "macos-intel-dmg": r"tableskim-[\d.]+-Darwin-x86_64\.dmg",
    "macos-intel-zip": r"tableskim-[\d.]+-Darwin-x86_64\.zip",
    "linux-deb": r"tableskim_[\d.]+_.+\.deb",
    "linux-rpm": r"tableskim-[\d.]+-.+\.rpm",
    "linux-zip": r"tableskim-[\d.]+-Linux-[^-]+\.zip",
    "linux-x11-zip": r"tableskim-[\d.]+-Linux-x11-[^-]+\.zip",
    "linux-tui-zip": r"tableskim-tui-[\d.]+-Linux-[^-]+\.zip",
}

PAGES = {"download.html": "en", "ja/download.html": "ja"}

CARD = re.compile(r'<article class="file" data-asset="([^"]+)">.*?</article>', re.S)


def format_date(iso, lang):
    date = datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).date()
    if lang == "ja":
        return f"{date.year}年{date.month}月{date.day}日"
    return f"{date:%B} {date.day}, {date.year}"


def replace_once(pattern, replacement, text, what):
    """Replaces the single match of `pattern`; the page must have it."""
    result, count = re.subn(pattern, lambda _: replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"fill-download-pages: {what} not found")
    return result


def fill_card(match, assets):
    card, key = match.group(0), match.group(1)
    if key not in PATTERNS:
        raise SystemExit(f"fill-download-pages: no pattern for {key}")
    pattern = re.compile(PATTERNS[key])
    asset = next((a for a in assets if pattern.fullmatch(a["name"])), None)
    if asset is None:
        print(f"fill-download-pages: the release has no file for {key}", file=sys.stderr)
        return card.replace('class="file"', 'class="file missing"', 1)
    card = replace_once(r'<code class="file-name">.*?</code>',
                        f'<code class="file-name">{html.escape(asset["name"])}</code>', card, f"{key} file name")
    card = replace_once(r'<span class="file-size"></span>',
                        f'<span class="file-size">{asset["size"] / 1048576:.1f} MB</span>', card, f"{key} size")
    card = replace_once(r'<a class="button primary" href="[^"]*">',
                        f'<a class="button primary" href="{html.escape(asset["browser_download_url"])}" download>',
                        card, f"{key} button")
    return card


def fill_page(text, release, lang):
    assets = release.get("assets", [])
    text = CARD.sub(lambda match: fill_card(match, assets), text)
    text = replace_once(r'<p class="release-info" id="release-info" hidden>',
                        '<p class="release-info" id="release-info">', text, "release info")
    text = replace_once(r'<b id="release-version"></b>',
                        f'<b id="release-version">{html.escape(release["tag_name"])}</b>', text, "version")
    text = replace_once(r'<span id="release-date"></span>',
                        f'<span id="release-date">{format_date(release["published_at"], lang)}</span>', text, "date")
    text = replace_once(r'<a id="release-notes" href="[^"]*">',
                        f'<a id="release-notes" href="{html.escape(release["html_url"])}">', text, "release notes")
    return text


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    release = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    site = Path(sys.argv[2])
    for page, lang in PAGES.items():
        path = site / page
        path.write_text(fill_page(path.read_text(encoding="utf-8"), release, lang), encoding="utf-8")
    print(f"Download pages filled in for {release['tag_name']}")


if __name__ == "__main__":
    main()
