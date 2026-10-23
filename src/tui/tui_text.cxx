#include "tui_text.hxx"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <ftxui/screen/string.hpp>

namespace
{
    std::string base64(std::string_view data)
    {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((data.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < data.size(); i += 3)
        {
            const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                               (static_cast<unsigned char>(data[i + 1]) << 8) |
                               static_cast<unsigned char>(data[i + 2]);
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += table[(n >> 6) & 63];
            out += table[n & 63];
        }
        if (i + 1 == data.size())
        {
            const unsigned n = static_cast<unsigned char>(data[i]) << 16;
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += "==";
        }
        else if (i + 2 == data.size())
        {
            const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                               (static_cast<unsigned char>(data[i + 1]) << 8);
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += table[(n >> 6) & 63];
            out += '=';
        }
        return out;
    }
} // namespace

std::string fitText(std::string_view text, int width, TextAlign align, bool ellipsis)
{
    if (width <= 0)
        return {};
    int text_width = ftxui::string_width(text);
    std::string out;
    if (text_width > width)
    {
        const int limit = ellipsis ? width - 1 : width;
        int used = 0;
        for (const std::string &glyph : ftxui::Utf8ToGlyphs(text))
        {
            const int glyph_width = ftxui::string_width(glyph);
            if (used + glyph_width > limit)
                break;
            out += glyph;
            used += glyph_width;
        }
        if (ellipsis)
        {
            out += "…";
            ++used;
        }
        // A wide character that did not fit leaves a gap.
        out.append(static_cast<size_t>(width - used), ' ');
        return out;
    }

    const int space = width - text_width;
    const int left = align == TextAlign::Right ? space : align == TextAlign::Center ? space / 2
                                                                                     : 0;
    out.append(static_cast<size_t>(left), ' ');
    out += text;
    out.append(static_cast<size_t>(space - left), ' ');
    return out;
}

std::vector<std::string> wrapText(std::string_view line, int width)
{
    std::vector<std::string> rows;
    width = std::max(width, 1);
    std::string row;
    int row_width = 0;
    // Position in `row` after the last space, and the width up to there.
    size_t break_at = std::string::npos;
    int break_width = 0;
    for (const std::string &glyph : ftxui::Utf8ToGlyphs(line))
    {
        const int glyph_width = ftxui::string_width(glyph);
        if (row_width + glyph_width > width)
        {
            if (break_at != std::string::npos && break_at > 0)
            {
                rows.push_back(row.substr(0, break_at));
                row.erase(0, break_at);
                row_width -= break_width;
            }
            else
            {
                rows.push_back(std::move(row));
                row.clear();
                row_width = 0;
            }
            break_at = std::string::npos;
        }
        row += glyph;
        row_width += glyph_width;
        if (glyph == " ")
        {
            break_at = row.size();
            break_width = row_width;
        }
    }
    rows.push_back(std::move(row));
    return rows;
}

std::string singleLine(std::string text)
{
    for (char &c : text)
    {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    return text;
}

void eraseLastCharacter(std::string &text)
{
    // Drop continuation bytes (10xxxxxx), then the lead byte.
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
        text.pop_back();
    if (!text.empty())
        text.pop_back();
}

void copyToClipboard(std::string_view text)
{
    std::string sequence = "\x1B]52;c;" + base64(text) + "\x07";
    // tmux passes the sequence through only when wrapped in DCS with every
    // ESC doubled (and set-clipboard / allow-passthrough enabled).
    if (std::getenv("TMUX"))
    {
        std::string wrapped = "\x1BPtmux;";
        for (char c : sequence)
        {
            if (c == '\x1B')
                wrapped += '\x1B';
            wrapped += c;
        }
        wrapped += "\x1B\\";
        sequence = std::move(wrapped);
    }
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
}
