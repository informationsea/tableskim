#ifndef _GUI_THEME_HXX_
#define _GUI_THEME_HXX_

#include <FL/Enumerations.H>
#include <cstddef>
#include <memory>
#include <string>

class Fl_Scrollbar;
class Fl_Widget;

// Name of the Fluent UI inspired theme accepted by setTheme().
inline constexpr const char *FLUENT_THEME = "fluent";

// Colors used by custom-drawn widgets (e.g. GuiTableView). They follow the
// current theme.
struct ThemeColors
{
    Fl_Color cell_background;
    Fl_Color cell_text;
    Fl_Color cell_selection;
    Fl_Color cell_search_hit;
    Fl_Color cell_modified; // edited, not saved yet
    Fl_Color cell_muted_text;
    Fl_Color grid_line;
    Fl_Color header_background;
    Fl_Color header_text;
    Fl_Color header_line;
};

// Applies FLUENT_THEME or one of the built-in FLTK schemes ("base",
// "plastic", "gtk+", "gleam", "oxy") and redraws all windows.
void setTheme(const char *name);
bool isFluentTheme();
const ThemeColors &themeColors();

// FLTK has no scheme hook for scrollbar colors, so widgets owning
// scrollbars call this before drawing them.
void styleScrollbar(Fl_Scrollbar *scrollbar);

enum class SortIndicator
{
    None,
    // Sortable but not sorted: both triangles, muted.
    Unsorted,
    Ascending,
    Descending,
};

// Width of the funnel drawn left of the sort icon on filtered columns.
inline constexpr int FILTER_ICON_WIDTH = 12;

// Width of the sort icon at the right end of a header cell, including the
// margin that keeps it clear of the column resize area. Clicks within it sort.
inline constexpr int SORT_ICON_WIDTH = 16;

// Draws a row or column header cell of a custom-drawn table. Classic schemes
// use a raised box in classic_color (e.g. Fl_Table::row_header_color()). A
// sort indicator is drawn in the rightmost SORT_ICON_WIDTH pixels, and a
// filtered column gets a funnel left of it.
void drawHeaderCell(const char *label, int x, int y, int w, int h, Fl_Color classic_color,
                    SortIndicator sort = SortIndicator::None, bool filtered = false);

// Widget labels treat '@' as the start of a symbol name, which drops the
// rest of the text, and menu items also treat '&' as a shortcut marker;
// doubling them draws them literally.
std::string escapeLabel(const std::string &text, bool menu = false);

// The tooltip of a status line, whose text changes while it may be shown.
// FLTK keeps a pointer to the text of the tooltip shown or about to be shown,
// so a new copy (copy_tooltip()) would leave that pointer dangling; the text
// goes into a buffer that never moves instead. Longer text is cut.
class ChangingTooltip
{
public:
    // No tooltip if `text` is empty.
    void set(Fl_Widget *widget, const std::string &text);

private:
    static constexpr size_t CAPACITY = 1024;
    std::unique_ptr<char[]> text_;
};

#endif // _GUI_THEME_HXX_
