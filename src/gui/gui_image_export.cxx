#include "gui_image_export.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_SVG_File_Surface.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_ask.H>
#include <FL/fl_utf8.h>
#include <algorithm>
#include <cctype>
#include <memory>

#include "spdlog/spdlog.h"

#include "file_util.hxx"
#include "gui_table_view.hxx"
#include "gui_theme.hxx"

namespace
{
    // The folder of the last image saved, for the next one.
    std::string &lastDirectory()
    {
        static std::string directory;
        return directory;
    }
} // namespace

std::string writeWidgetImage(Fl_Widget *widget, const std::filesystem::path &target)
{
    const int w = widget->w();
    const int h = widget->h();
    const std::string name = utf8String(target);
    if (lowerExtension(target) == ".svg")
    {
        FILE *file = fl_fopen(name.c_str(), "wb");
        if (!file)
            return "Cannot write " + name;
        Fl_SVG_File_Surface surface(w, h, file);
        Fl_Surface_Device::push_current(&surface);
        surface.draw(widget, 0, 0);
        Fl_Surface_Device::pop_current();
        if (surface.close() != 0)
            return "Cannot write " + name;
        return {};
    }
    Fl_Image_Surface surface(w, h);
    Fl_Surface_Device::push_current(&surface);
    surface.draw(widget, 0, 0);
    const std::unique_ptr<Fl_RGB_Image> image(surface.image());
    Fl_Surface_Device::pop_current();
    if (!image || fl_write_png(name.c_str(), image.get()) != 0)
        return "Cannot write " + name;
    return {};
}

void saveWidgetImage(Fl_Widget *widget, const std::string &file_name)
{
    static int last_filter = 0; // PNG, SVG
    const char *const extensions[] = {".png", ".svg"};
    Fl_Native_File_Chooser chooser;
    chooser.title("Save Image");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    chooser.filter("PNG Images\t*.png\nSVG Images\t*.svg");
    chooser.filter_value(last_filter);
    if (!lastDirectory().empty())
        chooser.directory(lastDirectory().c_str());
    const std::string preset = file_name + extensions[last_filter];
    chooser.preset_file(preset.c_str());
    int chosen;
    {
        NestedLoopGuard guard;
        chosen = chooser.show();
    }
    if (chosen == -1)
        spdlog::error("File chooser error: {}", chooser.errmsg());
    if (chosen != 0 || !chooser.filename())
        return;
    std::filesystem::path target = utf8Path(chooser.filename());
    const std::string ext = lowerExtension(target);
    if (ext == ".png" || ext == ".svg")
        last_filter = ext == ".svg" ? 1 : 0;
    else
    {
        const int filter = chooser.filter_value();
        last_filter = filter == 1 ? 1 : 0;
        target += extensions[last_filter];
        // Not checked by SAVEAS_CONFIRM, which saw the name without it.
        std::error_code exists_error;
        if (std::filesystem::exists(target, exists_error))
        {
            NestedLoopGuard guard;
            const std::string name = escapeLabel(utf8String(target.filename()));
            if (fl_choice("%s already exists. Replace it?", "Cancel", "Replace", nullptr, name.c_str()) != 1)
                return;
        }
    }
    lastDirectory() = utf8String(target.parent_path());
    if (const std::string error = writeWidgetImage(widget, target); !error.empty())
    {
        spdlog::error("{}", error);
        NestedLoopGuard guard;
        fl_alert("%s", escapeLabel(error).c_str());
    }
}

std::string imageFileName(const std::string &title)
{
    std::string name;
    for (const char c : title)
    {
        const bool refused = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                             c == '>' || c == '|' || (static_cast<unsigned char>(c) < 0x20);
        name += refused ? '_' : c;
    }
    return name;
}
