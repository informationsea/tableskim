#ifndef _MARKDOWN_HXX_
#define _MARKDOWN_HXX_

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The subset of GitHub Markdown that the user manual (docs/) uses: ATX
// headings, paragraphs, bullet and numbered lists (nested blocks indented),
// fenced code, pipe tables, `code`, **bold**, *italic*, [links](...),
// ![images](...) and raw <img> tags. The HTML is plain enough for FLTK's
// Fl_Help_View, which knows no CSS.

struct MarkdownImage
{
    std::string src;
    int width = 0;
    int height = 0;
    // Without a size in pixels: the share of the page width (in percent)
    // to show it at; 0 leaves the width to the browser.
    int percent = 0;
};

struct MarkdownHtmlOptions
{
    // The target to write for a link; empty shows the text without a link.
    std::function<std::string(const std::string &href)> link;
    // Where an image is and the size to show it at, given the share of the
    // page width it may take (100 for an image of its own); nullopt shows
    // the alt text instead.
    std::function<std::optional<MarkdownImage>(const std::string &src, int percent)> image;
    // Headings get GitHub's anchors as ids (see markdownHeadingAnchors())
    // instead of <a name="hN">.
    bool heading_ids = false;
    // Markup added to <head>, such as a style sheet.
    std::string head;
};

// Headings get <a name="h1">, <a name="h2">, ... in document order:
// Fl_Help_View cuts target names at 31 bytes, too few for non-ASCII headings.
std::string markdownToHtml(std::string_view markdown, const MarkdownHtmlOptions &options);

// GitHub's anchors for the headings, in document order: lower case, spaces
// as '-', punctuation dropped ("Running SQL" -> "running-sql"); a repeated
// anchor gets "-1", "-2", ... Index i belongs to <a name="h{i + 1}">.
std::vector<std::string> markdownHeadingAnchors(std::string_view markdown);

#endif // _MARKDOWN_HXX_
