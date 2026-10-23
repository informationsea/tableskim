#ifndef _TUI_TEXT_HXX_
#define _TUI_TEXT_HXX_

#include <string>
#include <string_view>
#include <vector>

enum class TextAlign
{
    Left,
    Center,
    Right,
};

// Pads or cuts the text to exactly `width` terminal columns. Cut text ends
// with "…" unless `ellipsis` is false. Wide (e.g. CJK) characters count as two.
std::string fitText(std::string_view text, int width, TextAlign align, bool ellipsis = true);

// Splits one line into rows of at most `width` terminal columns, breaking at
// spaces when possible and inside words that are too long.
std::vector<std::string> wrapText(std::string_view line, int width);

// Text for one line: line breaks and tabs become spaces (e.g. an Excel
// header cell with a line break as a column name).
std::string singleLine(std::string text);

// Removes the last UTF-8 character (Backspace in a text prompt).
void eraseLastCharacter(std::string &text);

// Asks the terminal to put the text on the system clipboard (OSC 52). Works
// over SSH and inside tmux, if the terminal allows it.
void copyToClipboard(std::string_view text);

#endif // _TUI_TEXT_HXX_
