#include "markdown.hxx"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>

namespace
{
    struct Context
    {
        const MarkdownHtmlOptions &options;
        size_t headings = 0;
        std::vector<std::string> anchors; // with options.heading_ids
        // Collects the heading lines as rendered (see headingLines()).
        std::vector<std::string_view> *heading_lines = nullptr;
    };

    bool isAsciiPunctuation(char c)
    {
        return std::ispunct(static_cast<unsigned char>(c)) != 0;
    }

    void appendEscaped(std::string &out, std::string_view text)
    {
        for (const char c : text)
        {
            switch (c)
            {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
            }
        }
    }

    std::string escaped(std::string_view text)
    {
        std::string out;
        appendEscaped(out, text);
        return out;
    }

    std::string_view trim(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        return text;
    }

    size_t indentOf(std::string_view line)
    {
        size_t n = 0;
        while (n < line.size() && line[n] == ' ')
            ++n;
        return n;
    }

    bool isBlank(std::string_view line)
    {
        return trim(line).empty();
    }

    // UTF-8 decoding of the first / last code point; 0 if there is none.
    char32_t firstCodePoint(std::string_view text)
    {
        if (text.empty())
            return 0;
        const auto b0 = static_cast<unsigned char>(text[0]);
        int length = b0 < 0x80 ? 1 : b0 < 0xE0 ? 2 : b0 < 0xF0 ? 3 : 4;
        if (static_cast<size_t>(length) > text.size())
            return 0;
        char32_t cp = length == 1 ? b0 : length == 2 ? (b0 & 0x1F) : length == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        for (int i = 1; i < length; ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        return cp;
    }

    char32_t lastCodePoint(std::string_view text)
    {
        size_t start = text.size();
        while (start > 0)
        {
            --start;
            if ((static_cast<unsigned char>(text[start]) & 0xC0) != 0x80)
                break;
        }
        return firstCodePoint(text.substr(start));
    }

    // East Asian wide characters, which are not separated by spaces.
    bool isWide(char32_t cp)
    {
        return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
               (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
               (cp >= 0xFFE0 && cp <= 0xFFE6) || cp >= 0x20000;
    }

    // Lines of a paragraph: a line break between two wide characters is
    // dropped (as CSS does for Japanese), any other becomes a space.
    std::string joinLines(const std::vector<std::string_view> &lines)
    {
        std::string text;
        for (const std::string_view raw : lines)
        {
            const std::string_view line = trim(raw);
            if (line.empty())
                continue;
            if (!text.empty() && !(isWide(lastCodePoint(text)) && isWide(firstCodePoint(line))))
                text += ' ';
            text += line;
        }
        return text;
    }

    // Punctuation and symbols that GitHub leaves out of heading anchors.
    bool isAnchorPunctuation(char32_t cp)
    {
        if (cp < 0x80)
            return !(std::isalnum(static_cast<int>(cp)) || cp == '-' || cp == '_' || cp == ' ');
        return (cp >= 0xA0 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2000 && cp <= 0x2BFF) ||
               (cp >= 0x3000 && cp <= 0x3004) || (cp >= 0x3008 && cp <= 0x3020) || cp == 0x3030 || cp == 0x30FB ||
               (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
               (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
    }

    struct InlineLink
    {
        size_t end = 0; // past the ')'
        std::string_view text;
        std::string_view target;
    };

    // "[text](target)" starting at `start` (the '['), if it is one.
    std::optional<InlineLink> parseLink(std::string_view text, size_t start)
    {
        int depth = 0;
        for (size_t i = start; i < text.size(); ++i)
        {
            if (text[i] == '\\')
            {
                ++i;
                continue;
            }
            if (text[i] == '`')
            {
                const size_t close = text.find('`', i + 1);
                if (close == std::string_view::npos)
                    return std::nullopt;
                i = close;
                continue;
            }
            if (text[i] == '[')
                ++depth;
            else if (text[i] == ']' && --depth == 0)
            {
                if (i + 1 >= text.size() || text[i + 1] != '(')
                    return std::nullopt;
                const size_t close = text.find(')', i + 2);
                if (close == std::string_view::npos)
                    return std::nullopt;
                return InlineLink{close + 1, text.substr(start + 1, i - start - 1), text.substr(i + 2, close - i - 2)};
            }
        }
        return std::nullopt;
    }

    // A code span starting at `start` (a backtick): its content and end.
    std::optional<std::pair<std::string_view, size_t>> parseCodeSpan(std::string_view text, size_t start)
    {
        size_t ticks = 0;
        while (start + ticks < text.size() && text[start + ticks] == '`')
            ++ticks;
        const std::string fence(ticks, '`');
        size_t search = start + ticks;
        while (true)
        {
            const size_t close = text.find(fence, search);
            if (close == std::string_view::npos)
                return std::nullopt;
            size_t after = close + ticks;
            if (after < text.size() && text[after] == '`')
            {
                // A longer run of backticks does not close the span.
                while (after < text.size() && text[after] == '`')
                    ++after;
                search = after;
                continue;
            }
            std::string_view content = text.substr(start + ticks, close - start - ticks);
            if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ')
                content = content.substr(1, content.size() - 2);
            return std::pair{content, after};
        }
    }

    std::string imageTag(const MarkdownImage &image, std::string_view alt)
    {
        std::string tag = "<img src=\"" + escaped(image.src) + "\"";
        if (image.width > 0 && image.height > 0)
            tag += " width=\"" + std::to_string(image.width) + "\" height=\"" + std::to_string(image.height) + "\"";
        else if (image.percent > 0)
            tag += " width=\"" + std::to_string(image.percent) + "%\"";
        tag += " alt=\"" + escaped(alt) + "\">";
        return tag;
    }

    std::string renderImage(std::string_view src, std::string_view alt, int percent, const Context &context)
    {
        const std::optional<MarkdownImage> image =
            context.options.image ? context.options.image(std::string(src), percent) : std::nullopt;
        if (!image)
            return escaped(alt);
        return imageTag(*image, alt);
    }

    // Inline Markdown as HTML (plain = false) or as the text a reader sees.
    std::string renderInline(std::string_view text, const Context &context, bool plain = false)
    {
        std::string out;
        bool bold = false;
        bool italic = false;
        for (size_t i = 0; i < text.size();)
        {
            const char c = text[i];
            if (c == '\\' && i + 1 < text.size() && isAsciiPunctuation(text[i + 1]))
            {
                plain ? void(out += text[i + 1]) : appendEscaped(out, text.substr(i + 1, 1));
                i += 2;
                continue;
            }
            if (c == '`')
            {
                if (const auto span = parseCodeSpan(text, i))
                {
                    if (plain)
                        out += span->first;
                    else
                        out += "<code>" + escaped(span->first) + "</code>";
                    i = span->second;
                    continue;
                }
            }
            if (c == '!' && i + 1 < text.size() && text[i + 1] == '[')
            {
                if (const auto link = parseLink(text, i + 1))
                {
                    out += plain ? std::string(link->text) : renderImage(link->target, link->text, 100, context);
                    i = link->end;
                    continue;
                }
            }
            if (c == '[')
            {
                if (const auto link = parseLink(text, i))
                {
                    const std::string inner = renderInline(link->text, context, plain);
                    const std::string href =
                        plain || !context.options.link ? std::string() : context.options.link(std::string(link->target));
                    out += href.empty() ? inner : "<a href=\"" + escaped(href) + "\">" + inner + "</a>";
                    i = link->end;
                    continue;
                }
            }
            if (c == '*' && i + 1 < text.size() && text[i + 1] == '*')
            {
                if (!plain)
                    out += bold ? "</b>" : "<b>";
                bold = !bold;
                i += 2;
                continue;
            }
            if (c == '*')
            {
                // Opens only before a word and with a closing '*' further on.
                const bool opens = !italic && i + 1 < text.size() && text[i + 1] != ' ' &&
                                   text.find('*', i + 1) != std::string_view::npos;
                if (opens || italic)
                {
                    if (!plain)
                        out += italic ? "</i>" : "<i>";
                    italic = !italic;
                    ++i;
                    continue;
                }
            }
            plain ? void(out += c) : appendEscaped(out, text.substr(i, 1));
            ++i;
        }
        if (!plain)
        {
            if (italic)
                out += "</i>";
            if (bold)
                out += "</b>";
        }
        return out;
    }

    std::optional<int> headingLevel(std::string_view line)
    {
        size_t level = 0;
        while (level < line.size() && line[level] == '#')
            ++level;
        if (level == 0 || level > 6 || level >= line.size() || line[level] != ' ')
            return std::nullopt;
        return static_cast<int>(level);
    }

    bool isFence(std::string_view line)
    {
        return trim(line).starts_with("```");
    }

    struct ListMarker
    {
        bool ordered = false;
        size_t width = 0; // of the marker with its space: content indent
    };

    std::optional<ListMarker> listMarker(std::string_view line)
    {
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ')
            return ListMarker{false, 2};
        size_t digits = 0;
        while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
            ++digits;
        if (digits > 0 && digits + 1 < line.size() && line[digits] == '.' && line[digits + 1] == ' ')
            return ListMarker{true, digits + 2};
        return std::nullopt;
    }

    bool isTableSeparator(std::string_view line)
    {
        static const std::regex separator(R"(^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$)");
        return line.find('-') != std::string_view::npos && std::regex_match(line.begin(), line.end(), separator);
    }

    std::vector<std::string> tableCells(std::string_view line)
    {
        line = trim(line);
        if (line.starts_with('|'))
            line.remove_prefix(1);
        if (line.ends_with('|') && !line.ends_with("\\|"))
            line.remove_suffix(1);
        std::vector<std::string> cells;
        std::string cell;
        for (size_t i = 0; i < line.size(); ++i)
        {
            // GitHub reads "\|" as a '|' inside the cell, also in code spans.
            if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '|')
            {
                cell += '|';
                ++i;
            }
            else if (line[i] == '|')
            {
                cells.emplace_back(trim(cell));
                cell.clear();
            }
            else
                cell += line[i];
        }
        cells.emplace_back(trim(cell));
        return cells;
    }

    // <img> tags of raw HTML, with the size and source the options give.
    std::string renderRawHtml(const std::string &html, const Context &context)
    {
        static const std::regex img(R"(<img\s+([^>]*)>)", std::regex::icase);
        static const std::regex attribute(R"re((\w+)\s*=\s*"([^"]*)")re");
        std::string out;
        auto last = html.cbegin();
        for (std::sregex_iterator it(html.begin(), html.end(), img), end; it != end; ++it)
        {
            out.append(last, (*it)[0].first);
            last = (*it)[0].second;
            std::map<std::string, std::string> attributes;
            const std::string list = (*it)[1].str();
            for (std::sregex_iterator a(list.begin(), list.end(), attribute); a != end; ++a)
            {
                std::string name = (*a)[1].str();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                attributes[name] = (*a)[2].str();
            }
            int percent = 100;
            if (const std::string &width = attributes["width"]; width.ends_with('%'))
                percent = std::clamp(std::atoi(width.c_str()), 1, 100);
            out += renderImage(attributes["src"], attributes["alt"], percent, context);
        }
        out.append(last, html.cend());
        return out;
    }

    void renderBlocks(const std::vector<std::string_view> &lines, Context &context, std::string &out, bool tight);

    // Lines [begin, end) without the first `indent` spaces (fewer if absent).
    std::vector<std::string_view> dedent(const std::vector<std::string_view> &lines, size_t begin, size_t end,
                                         size_t indent)
    {
        std::vector<std::string_view> result;
        for (size_t i = begin; i < end; ++i)
        {
            std::string_view line = lines[i];
            line.remove_prefix(std::min(indent, indentOf(line)));
            result.push_back(line);
        }
        return result;
    }

    // A list starting at lines[i]; returns the line after it.
    size_t renderList(const std::vector<std::string_view> &lines, size_t i, Context &context, std::string &out)
    {
        const bool ordered = listMarker(lines[i])->ordered;
        out += ordered ? "<ol>\n" : "<ul>\n";
        while (i < lines.size())
        {
            const auto marker = listMarker(lines[i]);
            if (!marker || marker->ordered != ordered)
                break;
            std::vector<std::string_view> item{lines[i].substr(marker->width)};
            size_t next = i + 1;
            while (next < lines.size())
            {
                if (isBlank(lines[next]))
                {
                    // A blank line continues the item if indented lines follow.
                    size_t after = next;
                    while (after < lines.size() && isBlank(lines[after]))
                        ++after;
                    if (after == lines.size() || indentOf(lines[after]) < marker->width)
                        break;
                }
                else if (indentOf(lines[next]) == 0)
                    break;
                ++next;
            }
            const std::vector<std::string_view> rest = dedent(lines, i + 1, next, marker->width);
            item.insert(item.end(), rest.begin(), rest.end());
            out += "<li>";
            renderBlocks(item, context, out, true);
            out += "</li>\n";
            i = next;
            while (i < lines.size() && isBlank(lines[i]))
            {
                // Blank lines between items; the list goes on after them.
                size_t after = i;
                while (after < lines.size() && isBlank(lines[after]))
                    ++after;
                if (after == lines.size() || !listMarker(lines[after]))
                    break;
                i = after;
            }
        }
        out += ordered ? "</ol>\n" : "</ul>\n";
        return i;
    }

    bool startsBlock(std::string_view line)
    {
        return headingLevel(line) || isFence(line) || listMarker(line) || trim(line).starts_with('|') ||
               line.starts_with('<');
    }

    void renderBlocks(const std::vector<std::string_view> &lines, Context &context, std::string &out, bool tight)
    {
        size_t i = 0;
        bool paragraph_before = false;
        while (i < lines.size())
        {
            const std::string_view line = lines[i];
            if (isBlank(line))
            {
                ++i;
                continue;
            }
            if (const auto level = headingLevel(line))
            {
                const std::string tag = "h" + std::to_string(*level);
                const size_t index = context.headings++;
                if (context.heading_lines)
                    context.heading_lines->push_back(line);
                if (context.options.heading_ids)
                {
                    const std::string anchor = index < context.anchors.size() ? context.anchors[index] : "";
                    out += "<" + tag + " id=\"" + escaped(anchor) + "\">";
                }
                else
                    out += "<a name=\"h" + std::to_string(index + 1) + "\"></a><" + tag + ">";
                out += renderInline(trim(line.substr(*level + 1)), context) + "</" + tag + ">\n";
                ++i;
                paragraph_before = false;
                continue;
            }
            if (isFence(line))
            {
                out += "<pre>";
                ++i;
                while (i < lines.size() && !isFence(lines[i]))
                {
                    appendEscaped(out, lines[i]);
                    out += '\n';
                    ++i;
                }
                out += "</pre>\n";
                ++i; // the closing fence
                paragraph_before = false;
                continue;
            }
            if (trim(line).starts_with('|') && i + 1 < lines.size() && isTableSeparator(lines[i + 1]))
            {
                out += "<table border=\"1\">\n<tr>";
                for (const std::string &cell : tableCells(line))
                    out += "<th>" + renderInline(cell, context) + "</th>";
                out += "</tr>\n";
                i += 2;
                while (i < lines.size() && trim(lines[i]).starts_with('|'))
                {
                    out += "<tr>";
                    for (const std::string &cell : tableCells(lines[i]))
                        out += "<td>" + renderInline(cell, context) + "</td>";
                    out += "</tr>\n";
                    ++i;
                }
                out += "</table>\n";
                paragraph_before = false;
                continue;
            }
            if (listMarker(line))
            {
                i = renderList(lines, i, context, out);
                paragraph_before = false;
                continue;
            }
            if (line.starts_with('<'))
            {
                std::string html;
                while (i < lines.size() && !isBlank(lines[i]))
                {
                    html += lines[i];
                    html += '\n';
                    ++i;
                }
                out += renderRawHtml(html, context);
                paragraph_before = false;
                continue;
            }
            std::vector<std::string_view> paragraph{line};
            ++i;
            while (i < lines.size() && !isBlank(lines[i]) && !startsBlock(lines[i]))
                paragraph.push_back(lines[i++]);
            const std::string text = renderInline(joinLines(paragraph), context);
            if (tight)
                out += (paragraph_before ? "<br><br>" : "") + text + "\n";
            else
                out += "<p>" + text + "</p>\n";
            paragraph_before = true;
        }
    }

    std::vector<std::string_view> splitLines(std::string_view text)
    {
        std::vector<std::string_view> lines;
        while (!text.empty())
        {
            const size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            lines.push_back(line);
            if (end == std::string_view::npos)
                break;
            text.remove_prefix(end + 1);
        }
        return lines;
    }

    // Heading lines in document order, as renderBlocks() numbers them: in
    // list items too, not in code or raw HTML blocks.
    std::vector<std::string_view> headingLines(const std::vector<std::string_view> &lines)
    {
        const MarkdownHtmlOptions options;
        std::vector<std::string_view> headings;
        Context context{options};
        context.heading_lines = &headings;
        std::string ignored;
        renderBlocks(lines, context, ignored, false);
        return headings;
    }
} // namespace

std::string markdownToHtml(std::string_view markdown, const MarkdownHtmlOptions &options)
{
    Context context{options};
    if (options.heading_ids)
        context.anchors = markdownHeadingAnchors(markdown);
    const std::vector<std::string_view> lines = splitLines(markdown);
    std::string title;
    for (const std::string_view heading : headingLines(lines))
    {
        if (*headingLevel(heading) == 1)
        {
            title = renderInline(trim(heading.substr(1)), context, true);
            break;
        }
    }
    std::string body;
    renderBlocks(lines, context, body, false);
    return "<html><head>" + options.head + "<title>" + escaped(title) + "</title></head><body>\n" + body + "</body></html>\n";
}

std::vector<std::string> markdownHeadingAnchors(std::string_view markdown)
{
    const MarkdownHtmlOptions options;
    Context context{options};
    std::vector<std::string> anchors;
    std::map<std::string, int> seen;
    for (const std::string_view heading : headingLines(splitLines(markdown)))
    {
        const std::string text = renderInline(trim(heading.substr(*headingLevel(heading) + 1)), context, true);
        std::string anchor;
        for (size_t i = 0; i < text.size();)
        {
            const std::string_view rest = std::string_view(text).substr(i);
            const char32_t cp = firstCodePoint(rest);
            size_t length = 1;
            while (i + length < text.size() && (static_cast<unsigned char>(text[i + length]) & 0xC0) == 0x80)
                ++length;
            if (cp == ' ')
                anchor += '-';
            else if (cp < 0x80 && !isAnchorPunctuation(cp))
                anchor += static_cast<char>(std::tolower(static_cast<int>(cp)));
            else if (cp >= 0x80 && !isAnchorPunctuation(cp))
                anchor += rest.substr(0, length);
            i += length;
        }
        const int count = seen[anchor]++;
        anchors.push_back(count == 0 ? anchor : anchor + "-" + std::to_string(count));
    }
    return anchors;
}
