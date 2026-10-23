#ifndef _HELP_DOCS_HXX_
#define _HELP_DOCS_HXX_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The user manual (docs/) as HTML pages for the GUI's help window. Paths
// are relative to docs/ with '/' separators: "en/gui.md", "images/x.png".
struct HelpFile
{
    std::string path;
    std::string data;
};

// Converts every "<lang>/<page>.md" to "<lang>/<page>.html" and keeps the
// other files (images) as they are. Links to other pages and to headings
// (GitHub anchors) point to the HTML pages and their <a name="hN">; web
// links stay; links to anything else become plain text. Images are scaled
// down to at most `max_image_width` pixels wide.
std::vector<HelpFile> buildHelpSite(const std::vector<HelpFile> &sources, int max_image_width);

// The manual for web browsers, installed with the program: like
// buildHelpSite(), but complete HTML5 pages with a style sheet, headings
// keep GitHub's anchors as ids, and images are scaled by the browser.
std::vector<HelpFile> buildBrowserManual(const std::vector<HelpFile> &sources);

// The manual's start page in a language ("en", "ja"): "ja/README.html".
std::string helpStartPage(std::string_view language);

// The width and height of a PNG image, from its header.
std::optional<std::pair<int, int>> pngSize(std::string_view data);

#endif // _HELP_DOCS_HXX_
