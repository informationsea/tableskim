#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "help_docs.hxx"
#include "markdown.hxx"

namespace
{
    const std::filesystem::path kDocs = TABLESKIM_DOCS_DIR;

    std::string toHtml(std::string_view markdown, const MarkdownHtmlOptions &options = {})
    {
        const std::string html = markdownToHtml(markdown, options);
        const size_t begin = html.find("<body>\n") + 7;
        return html.substr(begin, html.find("</body>") - begin);
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string png(int width, int height)
    {
        std::string data("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
        for (const int value : {width, height})
            for (int shift = 24; shift >= 0; shift -= 8)
                data += static_cast<char>((value >> shift) & 0xFF);
        return data + std::string(5, '\0');
    }
} // namespace

TEST_CASE("markdownToHtml renders blocks", "[markdown]")
{
    SECTION("headings get numbered anchors and the first one is the title")
    {
        const std::string html = markdownToHtml("# Title `x`\n\n## Part\n", {});
        CHECK(html.find("<title>Title x</title>") != std::string::npos);
        CHECK(html.find("<a name=\"h1\"></a><h1>Title <code>x</code></h1>") != std::string::npos);
        CHECK(html.find("<a name=\"h2\"></a><h2>Part</h2>") != std::string::npos);
    }

    SECTION("paragraph lines are joined, without a space between wide characters")
    {
        CHECK(toHtml("one\ntwo\n\nthree\n") == "<p>one two</p>\n<p>three</p>\n");
        CHECK(toHtml("日本語の\n文章と\nCSV ファイル\n") == "<p>日本語の文章と CSV ファイル</p>\n");
    }

    SECTION("fenced code is escaped and kept as is")
    {
        CHECK(toHtml("```sh\na <b> & **c**\n  d\n```\n") == "<pre>a &lt;b&gt; &amp; **c**\n  d\n</pre>\n");
    }

    SECTION("lists, with continuation lines and nested blocks")
    {
        CHECK(toHtml("- one\n  more\n- **two**\n") == "<ul>\n<li>one more\n</li>\n<li><b>two</b>\n</li>\n</ul>\n");
        CHECK(toHtml("1. a\n2. b\n") == "<ol>\n<li>a\n</li>\n<li>b\n</li>\n</ol>\n");
        CHECK(toHtml("- item:\n\n  ```sh\n  ls\n  ```\n\n- next\n") ==
              "<ul>\n<li>item:\n<pre>ls\n</pre>\n</li>\n<li>next\n</li>\n</ul>\n");
    }

    SECTION("tables, with \\| inside cells")
    {
        CHECK(toHtml("| Key | Action |\n| --- | :---: |\n| `\\|` | Freeze |\n") ==
              "<table border=\"1\">\n<tr><th>Key</th><th>Action</th></tr>\n"
              "<tr><td><code>|</code></td><td>Freeze</td></tr>\n</table>\n");
    }
}

TEST_CASE("markdownToHtml renders inline markup", "[markdown]")
{
    MarkdownHtmlOptions options;
    options.link = [](const std::string &href) { return href == "skip" ? std::string() : "to:" + href; };
    options.image = [](const std::string &src, int percent) -> std::optional<MarkdownImage>
    {
        if (src == "missing.png")
            return std::nullopt;
        return MarkdownImage{src, percent, 10};
    };

    CHECK(toHtml("*it* and **bold** and `a*b` and 1 < 2 & \\*", options) ==
          "<p><i>it</i> and <b>bold</b> and <code>a*b</code> and 1 &lt; 2 &amp; *</p>\n");
    CHECK(toHtml("[the `x` page](x.md#y) and [plain](skip)", options) ==
          "<p><a href=\"to:x.md#y\">the <code>x</code> page</a> and plain</p>\n");
    CHECK(toHtml("![A \"shot\"](a.png)", options) ==
          "<p><img src=\"a.png\" width=\"100\" height=\"10\" alt=\"A &quot;shot&quot;\"></p>\n");
    CHECK(toHtml("![Gone](missing.png)", options) == "<p>Gone</p>\n");
    CHECK(toHtml("<p>\n<img src=\"b.png\" alt=\"B\" width=\"49%\">\n</p>\n", options) ==
          "<p>\n<img src=\"b.png\" width=\"49\" height=\"10\" alt=\"B\">\n</p>\n");
    CHECK(toHtml("``a ` b``") == "<p><code>a ` b</code></p>\n");
}

TEST_CASE("markdownHeadingAnchors follows GitHub", "[markdown]")
{
    CHECK(markdownHeadingAnchors("# Using the GUI\n## The `tableskim` command on each platform\n"
                                 "## Running SQL\n```\n# not a heading\n```\n## Running SQL\n") ==
          std::vector<std::string>{"using-the-gui", "the-tableskim-command-on-each-platform", "running-sql",
                                   "running-sql-1"});
    CHECK(markdownHeadingAnchors("## How `=`, `≠` and `≥` compare\n## SQL の実行\n## 並べ替え・フィルタ（GUI）\n") ==
          std::vector<std::string>{"how---and--compare", "sql-の実行", "並べ替えフィルタgui"});
}

TEST_CASE("headings are numbered as they are rendered", "[markdown][regression]")
{
    // A heading in a list item is one; a "# " line in a raw HTML block is
    // not. Counted otherwise, ids shifted (and heading_ids read past the
    // anchors).
    const std::string markdown = "# Top\n\n- # In a list\n\n<div>\n# in html\n</div>\n\n## Last\n";
    CHECK(markdownHeadingAnchors(markdown) == std::vector<std::string>{"top", "in-a-list", "last"});
    MarkdownHtmlOptions options;
    options.heading_ids = true;
    const std::string html = markdownToHtml(markdown, options);
    CHECK(html.find("<h1 id=\"in-a-list\">") != std::string::npos);
    CHECK(html.find("<h2 id=\"last\">") != std::string::npos);
    const std::string named = markdownToHtml(markdown, {});
    CHECK(named.find("<a name=\"h3\"></a><h2>Last</h2>") != std::string::npos);
}

TEST_CASE("buildHelpSite links pages, anchors and images", "[markdown]")
{
    const std::vector<HelpFile> sources{
        {"en/README.md", "# Manual\n[GUI](gui.md#sorting), [top](#manual), [web](https://example.com), "
                         "[repo](../../README.md), [gone](gui.md#nothing)\n\n![Shot](../images/a.png)\n"},
        {"en/gui.md", "# GUI\n## Sorting\n"},
        {"images/a.png", png(1000, 500)},
    };
    const std::vector<HelpFile> site = buildHelpSite(sources, 400);
    REQUIRE(site.size() == 3);
    CHECK(site[0].path == "en/README.html");
    CHECK(site[0].data.find("<a href=\"gui.html#h2\">GUI</a>") != std::string::npos);
    CHECK(site[0].data.find("<a href=\"#h1\">top</a>") != std::string::npos);
    CHECK(site[0].data.find("<a href=\"https://example.com\">web</a>") != std::string::npos);
    CHECK(site[0].data.find(", repo,") != std::string::npos);
    CHECK(site[0].data.find("<a href=\"gui.html\">gone</a>") != std::string::npos);
    CHECK(site[0].data.find("<img src=\"../images/a.png\" width=\"400\" height=\"200\"") != std::string::npos);
    CHECK(site[1].path == "en/gui.html");
    CHECK(site[2].path == "images/a.png");
    CHECK(helpStartPage("ja") == "ja/README.html");
    CHECK(pngSize(png(3, 4)) == std::pair{3, 4});
    CHECK_FALSE(pngSize("not a png"));
}

TEST_CASE("buildBrowserManual writes HTML5 pages with GitHub anchors", "[markdown]")
{
    const std::vector<HelpFile> sources{
        {"README.md", "# Docs\n[English](en/README.md)\n"},
        {"en/README.md", "# Manual\n[GUI](gui.md#sorting), [top](#manual), [gone](gui.md#nothing)\n\n"
                         "![Shot](../images/a.png)\n<img src=\"../images/a.png\" alt=\"Half\" width=\"49%\">\n"},
        {"en/gui.md", "# GUI\n## Sorting\n"},
        {"images/a.png", png(1000, 500)},
    };
    const std::vector<HelpFile> site = buildBrowserManual(sources);
    REQUIRE(site.size() == 4);
    CHECK(site[0].path == "README.html");
    CHECK(site[0].data.starts_with("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"));
    CHECK(site[0].data.find("<a href=\"en/README.html\">English</a>") != std::string::npos);
    CHECK(site[1].data.starts_with("<!DOCTYPE html>\n<html lang=\"en\"><head>"));
    CHECK(site[1].data.find("<style>") != std::string::npos);
    CHECK(site[1].data.find("<h1 id=\"manual\">Manual</h1>") != std::string::npos);
    CHECK(site[1].data.find("<a href=\"gui.html#sorting\">GUI</a>") != std::string::npos);
    CHECK(site[1].data.find("<a href=\"#manual\">top</a>") != std::string::npos);
    CHECK(site[1].data.find("<a href=\"gui.html\">gone</a>") != std::string::npos);
    CHECK(site[1].data.find("<img src=\"../images/a.png\" alt=\"Shot\">") != std::string::npos);
    CHECK(site[1].data.find("<img src=\"../images/a.png\" width=\"49%\" alt=\"Half\">") != std::string::npos);
    CHECK(site[2].data.find("<h2 id=\"sorting\">Sorting</h2>") != std::string::npos);
    CHECK(site[3].path == "images/a.png");
}

// Every link of the manual must lead to a page and heading of the manual
// (or the web), and every image must be found, in each language.
TEST_CASE("The user manual converts with every link resolved", "[markdown]")
{
    std::vector<HelpFile> sources;
    for (const char *directory : {"en", "ja", "images"})
    {
        for (const auto &entry : std::filesystem::directory_iterator(kDocs / directory))
        {
            const std::string name = entry.path().filename().string();
            if (name.ends_with(".md") || name.ends_with(".png"))
                sources.push_back({std::string(directory) + "/" + name, readFile(entry.path())});
        }
    }
    // Fragments are dropped quietly when converting, so check them here.
    const std::regex fragment_link(R"(\]\(([^)#\s]*)#([^)\s]+)\))");
    for (const HelpFile &source : sources)
    {
        if (!source.path.ends_with(".md"))
            continue;
        const std::string directory = source.path.substr(0, source.path.find('/') + 1);
        for (std::sregex_iterator it(source.data.begin(), source.data.end(), fragment_link), end; it != end; ++it)
        {
            const std::string file = (*it)[1].str();
            if (file.starts_with("../../") || file.starts_with("https://"))
                continue;
            const std::string page = file.empty() ? source.path : directory + file;
            const auto target = std::find_if(sources.begin(), sources.end(),
                                             [&page](const HelpFile &f) { return f.path == page; });
            INFO(source.path << " -> " << (*it)[0].str());
            REQUIRE(target != sources.end());
            const std::vector<std::string> anchors = markdownHeadingAnchors(target->data);
            CHECK(std::find(anchors.begin(), anchors.end(), (*it)[2].str()) != anchors.end());
        }
    }

    const std::vector<HelpFile> site = buildHelpSite(sources, 800);
    std::map<std::string, std::string> pages;
    std::set<std::string> files;
    for (const HelpFile &file : site)
    {
        files.insert(file.path);
        if (file.path.ends_with(".html"))
            pages[file.path] = file.data;
    }
    for (const char *language : {"en", "ja"})
        CHECK(pages.contains(helpStartPage(language)));

    // A Markdown link that was not converted would still show its syntax.
    const std::regex markdown_link(R"(\]\([^)]*\))");
    const std::regex href(R"re(<a href="([^"]*)">)re");
    const std::regex src(R"re(<img src="([^"]*)" width="\d+" height="\d+")re");
    for (const auto &[path, html] : pages)
    {
        INFO(path);
        CHECK_FALSE(std::regex_search(html, markdown_link));
        CHECK(html.find("<img src") == html.find("<img src=\"../images/")); // every image is sized below
        const std::string directory = path.substr(0, path.find('/') + 1);
        for (std::sregex_iterator it(html.begin(), html.end(), href), end; it != end; ++it)
        {
            const std::string target = (*it)[1].str();
            INFO(target);
            if (target.starts_with("https://"))
                continue;
            const size_t hash = target.find('#');
            const std::string page = hash == 0 ? path : directory + target.substr(0, hash);
            REQUIRE(pages.contains(page));
            if (hash != std::string::npos)
                CHECK(pages[page].find("<a name=\"" + target.substr(hash + 1) + "\">") != std::string::npos);
        }
        size_t images = 0;
        for (std::sregex_iterator it(html.begin(), html.end(), src), end; it != end; ++it, ++images)
            CHECK(files.contains("images/" + (*it)[1].str().substr(std::string("../images/").size())));
        size_t tags = 0;
        for (size_t at = html.find("<img "); at != std::string::npos; at = html.find("<img ", at + 1))
            ++tags;
        CHECK(images == tags);
    }

    // The browser manual, with the language index on top, links the same way.
    sources.push_back({"README.md", readFile(kDocs / "README.md")});
    std::map<std::string, std::string> browser_pages;
    for (const HelpFile &file : buildBrowserManual(sources))
    {
        if (file.path.ends_with(".html"))
            browser_pages[file.path] = file.data;
    }
    CHECK(browser_pages.contains("README.html"));
    for (const auto &[path, html] : browser_pages)
    {
        INFO(path);
        CHECK(html.starts_with("<!DOCTYPE html>"));
        CHECK_FALSE(std::regex_search(html, markdown_link));
        const std::filesystem::path directory = std::filesystem::path(path).parent_path();
        for (std::sregex_iterator it(html.begin(), html.end(), href), end; it != end; ++it)
        {
            const std::string target = (*it)[1].str();
            INFO(target);
            if (target.starts_with("https://"))
                continue;
            const size_t hash = target.find('#');
            const std::string page =
                hash == 0 ? path : (directory / target.substr(0, hash)).lexically_normal().generic_string();
            REQUIRE(browser_pages.contains(page));
            if (hash != std::string::npos)
                CHECK(browser_pages[page].find(" id=\"" + target.substr(hash + 1) + "\">") != std::string::npos);
        }
    }
}
