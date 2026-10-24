#include "gui_theme.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cstring>

namespace
{
    // Fluent 2 (light) design tokens.
    const Fl_Color NEUTRAL_BACKGROUND = fl_rgb_color(0xF5, 0xF5, 0xF5);
    const Fl_Color NEUTRAL_BACKGROUND_WHITE = fl_rgb_color(0xFF, 0xFF, 0xFF);
    const Fl_Color NEUTRAL_FOREGROUND = fl_rgb_color(0x24, 0x24, 0x24);
    const Fl_Color NEUTRAL_FOREGROUND_2 = fl_rgb_color(0x42, 0x42, 0x42);
    const Fl_Color NEUTRAL_FOREGROUND_DISABLED = fl_rgb_color(0x9E, 0x9E, 0x9E);
    const Fl_Color NEUTRAL_STROKE = fl_rgb_color(0xD1, 0xD1, 0xD1);
    const Fl_Color NEUTRAL_STROKE_SUBTLE = fl_rgb_color(0xE0, 0xE0, 0xE0);
    const Fl_Color NEUTRAL_STROKE_ACCESSIBLE = fl_rgb_color(0x61, 0x61, 0x61);
    const Fl_Color BRAND_BACKGROUND = fl_rgb_color(0x0F, 0x6C, 0xBD);
    const Fl_Color BRAND_BACKGROUND_SUBTLE = fl_rgb_color(0xEB, 0xF3, 0xFC);
    // Light yellow, like search highlights elsewhere; dark text stays readable on it.
    const Fl_Color SEARCH_HIT_BACKGROUND = fl_rgb_color(0xFF, 0xEB, 0x99);
    // Light green for edited cells, apart from the selection and search hits.
    const Fl_Color MODIFIED_BACKGROUND = fl_rgb_color(0xDF, 0xF6, 0xDD);
    const Fl_Color SCROLLBAR_THUMB = fl_rgb_color(0x9E, 0x9E, 0x9E);
    const int CORNER_RADIUS = 4;
    const Fl_Boxtype FLUENT_SCROLLBAR_THUMB_BOX = FL_FREE_BOXTYPE;

    const ThemeColors FLUENT_COLORS = {
        NEUTRAL_BACKGROUND_WHITE,
        NEUTRAL_FOREGROUND,
        BRAND_BACKGROUND_SUBTLE,
        SEARCH_HIT_BACKGROUND,
        MODIFIED_BACKGROUND,
        NEUTRAL_FOREGROUND_DISABLED,
        NEUTRAL_STROKE_SUBTLE,
        NEUTRAL_BACKGROUND,
        NEUTRAL_FOREGROUND_2,
        NEUTRAL_STROKE,
    };

    const ThemeColors CLASSIC_COLORS = {
        FL_WHITE,
        FL_BLACK,
        FL_BACKGROUND_COLOR,
        SEARCH_HIT_BACKGROUND,
        MODIFIED_BACKGROUND,
        FL_DARK2,
        FL_LIGHT2,
        FL_BACKGROUND_COLOR,
        FL_BLACK,
        FL_DARK3,
    };

    bool fluent_active = false;

    // FLTK's own colors, restored when switching back to a built-in scheme.
    struct SavedColors
    {
        bool saved = false;
        unsigned char background[3];
        unsigned char background2[3];
        unsigned char foreground[3];
        Fl_Color selection;
    } saved_colors;

    void saveColors()
    {
        if (saved_colors.saved)
            return;
        saved_colors.saved = true;
        Fl::get_color(FL_BACKGROUND_COLOR, saved_colors.background[0], saved_colors.background[1], saved_colors.background[2]);
        Fl::get_color(FL_BACKGROUND2_COLOR, saved_colors.background2[0], saved_colors.background2[1], saved_colors.background2[2]);
        Fl::get_color(FL_FOREGROUND_COLOR, saved_colors.foreground[0], saved_colors.foreground[1], saved_colors.foreground[2]);
        saved_colors.selection = Fl::get_color(FL_SELECTION_COLOR);
    }

    void restoreColors()
    {
        if (!saved_colors.saved)
            return;
        Fl::background(saved_colors.background[0], saved_colors.background[1], saved_colors.background[2]);
        Fl::background2(saved_colors.background2[0], saved_colors.background2[1], saved_colors.background2[2]);
        Fl::foreground(saved_colors.foreground[0], saved_colors.foreground[1], saved_colors.foreground[2]);
        Fl::set_color(FL_SELECTION_COLOR, saved_colors.selection);
    }

    void setRgb(void (*setter)(uchar, uchar, uchar), Fl_Color c)
    {
        uchar r, g, b;
        Fl::get_color(c, r, g, b);
        setter(r, g, b);
    }

    int radiusFor(int w, int h)
    {
        return std::max(0, std::min(CORNER_RADIUS, std::min(w, h) / 2));
    }

    // Raised controls (buttons, tabs, menus): flat fill with a hairline stroke.
    void fluentUpBox(int x, int y, int w, int h, Fl_Color c)
    {
        const int r = radiusFor(w, h);
        fl_color(Fl::box_color(c));
        fl_rounded_rectf(x, y, w, h, r);
        fl_color(Fl::box_color(NEUTRAL_STROKE));
        fl_rounded_rect(x, y, w, h, r);
    }

    // Sunken controls (inputs, pressed buttons, scrollbar tracks). Text
    // fields get Fluent's darker bottom stroke.
    void fluentDownBox(int x, int y, int w, int h, Fl_Color c)
    {
        fluentUpBox(x, y, w, h, c);
        if (c == FL_BACKGROUND2_COLOR && w > 2 * CORNER_RADIUS && h > 2)
        {
            fl_color(Fl::box_color(NEUTRAL_STROKE_ACCESSIBLE));
            fl_xyline(x + CORNER_RADIUS / 2, y + h - 1, x + w - 1 - CORNER_RADIUS / 2);
        }
    }

    void fluentFrame(int x, int y, int w, int h, Fl_Color)
    {
        fl_color(Fl::box_color(NEUTRAL_STROKE));
        fl_rect(x, y, w, h);
    }

    void fluentRoundBox(int x, int y, int w, int h, Fl_Color c)
    {
        fl_color(Fl::box_color(c));
        fl_pie(x, y, w, h, 0.0, 360.0);
        fl_color(Fl::box_color(NEUTRAL_STROKE_ACCESSIBLE));
        fl_arc(x, y, w, h, 0.0, 360.0);
    }

    // Fl_Scrollbar draws its arrow buttons with the thumb's boxtype too; the
    // buttons are always square, so those are kept flat.
    void fluentScrollbarThumbBox(int x, int y, int w, int h, Fl_Color)
    {
        if (w == h)
        {
            fl_color(Fl::box_color(FL_BACKGROUND_COLOR));
            fl_rectf(x, y, w, h);
            return;
        }
        const int inset = std::min(w, h) / 4;
        x += inset;
        y += inset;
        w -= 2 * inset;
        h -= 2 * inset;
        fl_color(Fl::box_color(SCROLLBAR_THUMB));
        fl_rounded_rectf(x, y, w, h, std::min(w, h) / 2);
    }

    void applyFluentBoxtypes()
    {
        Fl::set_boxtype(FL_UP_FRAME, fluentFrame, 1, 1, 2, 2);
        Fl::set_boxtype(FL_DOWN_FRAME, fluentFrame, 1, 1, 2, 2);
        Fl::set_boxtype(FL_THIN_UP_FRAME, fluentFrame, 1, 1, 2, 2);
        Fl::set_boxtype(FL_THIN_DOWN_FRAME, fluentFrame, 1, 1, 2, 2);

        Fl::set_boxtype(FL_UP_BOX, fluentUpBox, 1, 1, 2, 2);
        Fl::set_boxtype(FL_DOWN_BOX, fluentDownBox, 1, 1, 2, 2);
        Fl::set_boxtype(FL_THIN_UP_BOX, fluentUpBox, 1, 1, 2, 2);
        Fl::set_boxtype(FL_THIN_DOWN_BOX, fluentDownBox, 1, 1, 2, 2);
        Fl::set_boxtype(_FL_ROUND_UP_BOX, fluentRoundBox, 2, 2, 4, 4);
        Fl::set_boxtype(_FL_ROUND_DOWN_BOX, fluentRoundBox, 2, 2, 4, 4);
        Fl::set_boxtype(FLUENT_SCROLLBAR_THUMB_BOX, fluentScrollbarThumbBox, 0, 0, 0, 0);
    }

    void applyFluentColors()
    {
        setRgb(Fl::background, NEUTRAL_BACKGROUND);
        setRgb(Fl::background2, NEUTRAL_BACKGROUND_WHITE);
        setRgb(Fl::foreground, NEUTRAL_FOREGROUND);
        Fl::set_color(FL_SELECTION_COLOR, BRAND_BACKGROUND);
    }
} // namespace

void setTheme(const char *name)
{
    saveColors();
    fluent_active = name && std::strcmp(name, FLUENT_THEME) == 0;
    if (fluent_active)
    {
        // Start from the plain scheme (no background tile), then replace its boxes.
        Fl::scheme("base");
        applyFluentColors();
        applyFluentBoxtypes();
        Fl::scrollbar_size(14);
    }
    else
    {
        restoreColors();
        Fl::scheme(name); // also restores the boxtypes
    }

    for (Fl_Window *win = Fl::first_window(); win; win = Fl::next_window(win))
        win->redraw();
}

bool isFluentTheme()
{
    return fluent_active;
}

const ThemeColors &themeColors()
{
    return fluent_active ? FLUENT_COLORS : CLASSIC_COLORS;
}

void styleScrollbar(Fl_Scrollbar *scrollbar)
{
    if (fluent_active)
    {
        scrollbar->color(FL_BACKGROUND_COLOR);
        scrollbar->slider(FLUENT_SCROLLBAR_THUMB_BOX);
        scrollbar->labelcolor(SCROLLBAR_THUMB);
    }
    else
    {
        // Fl_Scrollbar's defaults.
        scrollbar->color(FL_DARK2);
        scrollbar->slider(FL_UP_BOX);
        scrollbar->labelcolor(FL_FOREGROUND_COLOR);
    }
}

void drawHeaderCell(const char *label, int x, int y, int w, int h, Fl_Color classic_color, SortIndicator sort,
                    bool filtered, const char *mark)
{
    const ThemeColors &colors = themeColors();
    fl_push_clip(x, y, w, h);
    if (fluent_active)
    {
        // Flat header separated by hairlines, like Fluent's DataGrid.
        fl_color(colors.header_background);
        fl_rectf(x, y, w, h);
        fl_color(colors.header_line);
        fl_xyline(x, y + h - 1, x + w - 1);
        fl_yxline(x + w - 1, y, y + h - 1);
    }
    else
    {
        fl_draw_box(FL_THIN_UP_BOX, x, y, w, h, classic_color);
    }
    const int mark_width = mark && *mark ? TYPE_MARK_WIDTH : 0;
    if (mark_width > 0)
    {
        fl_color(colors.cell_muted_text);
        fl_draw(mark, x + 4, y, mark_width, h, FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    }
    fl_color(colors.header_text);
    // The label keeps clear of the indicator.
    const int indicator_width = sort == SortIndicator::None ? 0 : SORT_ICON_WIDTH;
    const int filter_width = filtered ? FILTER_ICON_WIDTH : 0;
    fl_draw(label, x + 4 + mark_width, y, w - 8 - mark_width - indicator_width - filter_width, h,
            FL_ALIGN_CENTER | FL_ALIGN_CLIP, nullptr, 0);
    if (filtered)
    {
        const int cx = x + w - indicator_width - filter_width / 2 - 2;
        const int cy = y + h / 2;
        fl_polygon(cx - 4, cy - 4, cx + 4, cy - 4, cx, cy + 1);
        fl_rectf(cx - 1, cy, 2, 4);
    }
    if (sort != SortIndicator::None)
    {
        // Drawn rather than "▲"/"▼" so that it does not depend on the font.
        const int cx = x + w - indicator_width / 2 - 2;
        const int cy = y + h / 2;
        switch (sort)
        {
        case SortIndicator::Unsorted:
            fl_color(colors.cell_muted_text);
            fl_polygon(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy - 7);
            fl_polygon(cx - 4, cy + 2, cx + 4, cy + 2, cx, cy + 7);
            break;
        case SortIndicator::Ascending:
            fl_polygon(cx - 4, cy + 2, cx + 4, cy + 2, cx, cy - 3);
            break;
        default:
            fl_polygon(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            break;
        }
    }
    fl_pop_clip();
}

std::string escapeLabel(const std::string &text, bool menu)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text)
    {
        if (c == '@' || (menu && c == '&'))
            escaped += c;
        escaped += c;
    }
    return escaped;
}

void ChangingTooltip::set(Fl_Widget *widget, const std::string &text)
{
    if (!text_)
        text_ = std::make_unique<char[]>(CAPACITY);
    // Cut on a character boundary.
    size_t size = std::min(text.size(), CAPACITY - 1);
    while (size > 0 && size < text.size() && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80)
        --size;
    std::memcpy(text_.get(), text.data(), size);
    text_[size] = '\0';
    // An empty text also hides a tooltip about to be shown.
    widget->tooltip(size > 0 ? text_.get() : nullptr);
    if (Fl_Tooltip::current() == widget)
    {
        if (Fl_Window *window = Fl_Tooltip::current_window(); window && window->visible())
            window->redraw();
    }
}
