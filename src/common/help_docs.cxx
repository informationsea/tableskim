#include "help_docs.hxx"

#include <algorithm>
#include <cstdint>
#include <map>

#include "markdown.hxx"

namespace
{
    std::string directoryOf(std::string_view path)
    {
        const size_t slash = path.rfind('/');
        return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
    }

    // `relative` seen from `directory`, with "." and ".." resolved; nullopt
    // if it leaves docs/.
    std::optional<std::string> resolvePath(const std::string &directory, std::string_view relative)
    {
        std::vector<std::string> parts;
        auto add = [&parts](std::string_view path)
        {
            while (!path.empty())
            {
                const size_t slash = path.find('/');
                const std::string_view part = path.substr(0, slash);
                if (part == "..")
                {
                    if (parts.empty())
                        return false;
                    parts.pop_back();
                }
                else if (!part.empty() && part != ".")
                    parts.emplace_back(part);
                if (slash == std::string_view::npos)
                    break;
                path.remove_prefix(slash + 1);
            }
            return true;
        };
        if (!add(directory) || !add(relative))
            return std::nullopt;
        std::string result;
        for (const std::string &part : parts)
            result += (result.empty() ? "" : "/") + part;
        return result;
    }

    std::string htmlPath(std::string_view markdown_path)
    {
        return std::string(markdown_path.substr(0, markdown_path.size() - 3)) + ".html";
    }

    uint32_t bigEndian32(std::string_view data, size_t offset)
    {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i)
            value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
        return value;
    }
} // namespace

std::optional<std::pair<int, int>> pngSize(std::string_view data)
{
    // The signature, then the IHDR chunk: length, type, width, height.
    static constexpr std::string_view SIGNATURE("\x89PNG\r\n\x1a\n", 8);
    if (data.size() < 24 || !data.starts_with(SIGNATURE) || data.substr(12, 4) != "IHDR")
        return std::nullopt;
    const uint32_t width = bigEndian32(data, 16);
    const uint32_t height = bigEndian32(data, 20);
    if (width == 0 || height == 0 || width > 1u << 20 || height > 1u << 20)
        return std::nullopt;
    return std::pair{static_cast<int>(width), static_cast<int>(height)};
}

std::string helpStartPage(std::string_view language)
{
    return std::string(language) + "/README.html";
}

namespace
{
    // Every browser page carries it, so that a page opened on its own looks
    // the same; light and dark follow the system.
    const char *const BROWSER_HEAD = R"(<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root { color-scheme: light dark; }
body { max-width: 60em; margin: 0 auto; padding: 1em 16px 3em; line-height: 1.6;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Hiragino Sans", "Yu Gothic UI", "Noto Sans",
    "Noto Sans CJK JP", sans-serif; }
h1, h2 { border-bottom: 1px solid #8884; padding-bottom: 0.2em; }
a { color: #2f6fd0; }
@media (prefers-color-scheme: dark) { a { color: #7fb0ff; } }
img { max-width: 100%; height: auto; vertical-align: top; }
table { border-collapse: collapse; border: none; margin: 1em 0; display: block; overflow-x: auto; }
th, td { border: 1px solid #8888; padding: 0.3em 0.6em; text-align: left; vertical-align: top; }
th { background: #8881; }
code, pre { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Noto Sans Mono CJK JP", monospace;
  font-size: 0.9em; background: #8882; border-radius: 4px; }
code { padding: 0.1em 0.3em; }
pre { padding: 0.8em 1em; overflow-x: auto; line-height: 1.4; }
</style>
)";

    // max_image_width is ignored for a browser.
    std::vector<HelpFile> buildSite(const std::vector<HelpFile> &sources, bool browser, int max_image_width)
    {
        std::map<std::string, const HelpFile *> files;
        std::map<std::string, std::vector<std::string>> anchors; // of each page
        for (const HelpFile &file : sources)
        {
            files[file.path] = &file;
            if (file.path.ends_with(".md"))
                anchors[file.path] = markdownHeadingAnchors(file.data);
        }
        // The target of a GitHub anchor on a page: the anchor itself in a
        // browser, otherwise its <a name="hN">; empty if there is none.
        auto target = [&anchors, browser](const std::string &page, std::string_view anchor) -> std::string
        {
            const std::vector<std::string> &names = anchors[page];
            const auto it = std::find(names.begin(), names.end(), anchor);
            if (it == names.end())
                return {};
            return browser ? *it : "h" + std::to_string(it - names.begin() + 1);
        };

        std::vector<HelpFile> site;
        for (const HelpFile &file : sources)
        {
            if (!file.path.ends_with(".md"))
            {
                site.push_back(file);
                continue;
            }
            const std::string directory = directoryOf(file.path);
            MarkdownHtmlOptions options;
            options.link = [&](const std::string &href) -> std::string
            {
                if (href.starts_with("http://") || href.starts_with("https://") || href.starts_with("mailto:"))
                    return href;
                const size_t hash = href.find('#');
                const std::string page_part = href.substr(0, hash);
                const std::string anchor = hash == std::string::npos ? std::string() : href.substr(hash + 1);
                if (page_part.empty())
                {
                    const std::string name = target(file.path, anchor);
                    return name.empty() ? std::string() : "#" + name;
                }
                const std::optional<std::string> page = resolvePath(directory, page_part);
                if (!page || !page->ends_with(".md") || !anchors.contains(*page))
                    return {};
                std::string result = htmlPath(page_part);
                if (const std::string name = target(*page, anchor); !name.empty())
                    result += "#" + name;
                return result;
            };
            options.image = [&](const std::string &src, int percent) -> std::optional<MarkdownImage>
            {
                const std::optional<std::string> path = resolvePath(directory, src);
                const auto it = path ? files.find(*path) : files.end();
                if (it == files.end())
                    return std::nullopt;
                MarkdownImage image{src};
                if (browser)
                    image.percent = percent < 100 ? percent : 0;
                else if (const auto size = pngSize(it->second->data))
                {
                    image.width = std::min(size->first, std::max(1, max_image_width * percent / 100));
                    image.height = std::max(1, static_cast<int>(static_cast<long long>(size->second) * image.width /
                                                                 size->first));
                }
                return image;
            };
            if (!browser)
            {
                site.push_back({htmlPath(file.path), markdownToHtml(file.data, options)});
                continue;
            }
            options.heading_ids = true;
            options.head = BROWSER_HEAD;
            // The language is the directory: "ja/gui.md".
            const std::string language = directory.substr(0, directory.find('/'));
            const std::string html = markdownToHtml(file.data, options);
            site.push_back({htmlPath(file.path),
                            "<!DOCTYPE html>\n<html" + (language.empty() ? "" : " lang=\"" + language + "\"") +
                                html.substr(std::string_view("<html").size())});
        }
        return site;
    }
} // namespace

std::vector<HelpFile> buildHelpSite(const std::vector<HelpFile> &sources, int max_image_width)
{
    return buildSite(sources, false, max_image_width);
}

std::vector<HelpFile> buildBrowserManual(const std::vector<HelpFile> &sources)
{
    return buildSite(sources, true, 0);
}
