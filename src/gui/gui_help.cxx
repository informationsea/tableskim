#include "gui_help.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/fl_ask.H>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "spdlog/spdlog.h"

#include "gui_table_view.hxx"
#include "help_docs.hxx"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

// The manual's files, embedded by cmake/EmbedFiles.cmake.
struct EmbeddedHelpFile
{
    const char *path;
    const unsigned char *data;
    size_t size;
};
extern const EmbeddedHelpFile EMBEDDED_HELP_FILES[];
extern const size_t EMBEDDED_HELP_FILE_COUNT;

namespace
{
    const int WINDOW_WIDTH = 900;
    const int WINDOW_HEIGHT = 720;
    const int MARGIN = 8;
    const int BUTTON_WIDTH = 80;
    const int BUTTON_HEIGHT = 25;
    const int LANGUAGE_WIDTH = 110;
    const int FONT_SIZE = 14;
    // Screenshots are scaled down to this width (pixels).
    const int MAX_IMAGE_WIDTH = 760;

    struct Language
    {
        const char *code; // the directory under docs/
        const char *label;
    };
    const Language LANGUAGES[] = {{"en", "English"}, {"ja", "日本語"}};

    bool systemPrefersJapanese()
    {
#ifdef _WIN32
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
#elif defined(__APPLE__)
        bool japanese = false;
        if (CFArrayRef languages = CFLocaleCopyPreferredLanguages())
        {
            char code[32];
            if (CFArrayGetCount(languages) > 0 &&
                CFStringGetCString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0)), code, sizeof(code),
                                   kCFStringEncodingUTF8))
                japanese = std::string_view(code).starts_with("ja");
            CFRelease(languages);
        }
        return japanese;
#else
        // The first one set wins, as for gettext.
        for (const char *name : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"})
        {
            const char *value = std::getenv(name);
            if (value && *value)
                return std::string_view(value).starts_with("ja");
        }
        return false;
#endif
    }

    // Fl_Help_View takes UTF-8 paths with '/' separators on every platform.
    std::string viewPath(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    class HelpWindow
    {
    public:
        HelpWindow();
        ~HelpWindow();
        HelpWindow(const HelpWindow &) = delete;
        HelpWindow &operator=(const HelpWindow &) = delete;

        void show();

    private:
        // A page left by following a link, to come back to.
        struct Place
        {
            std::string file;
            int top = 0;
        };

        std::unique_ptr<Fl_Double_Window> window_;
        Fl_Help_View *view_ = nullptr;
        Fl_Button *back_ = nullptr;
        Fl_Button *forward_ = nullptr;
        Fl_Choice *language_ = nullptr;
        std::filesystem::path directory_; // the pages, once written
        std::vector<Place> back_places_;
        std::vector<Place> forward_places_;
        bool going_back_or_forward_ = false;
        std::string title_;

        bool writePages();
        std::string languageCode() const;
        void load(const std::string &file, int top = 0);
        void go(std::vector<Place> &from, std::vector<Place> &to);
        void showContents();
        void switchLanguage();
        void updateWindow();

        static const char *linkCallback(Fl_Widget *widget, const char *uri);
    };

    std::unique_ptr<HelpWindow> help_window;
} // namespace

HelpWindow::HelpWindow()
{
    // Fl_Shared_Image needs the PNG loader for the screenshots.
    fl_register_images();
    window_ = std::make_unique<Fl_Double_Window>(WINDOW_WIDTH, WINDOW_HEIGHT, "TableSkim User Manual");

    auto *bar = new Fl_Group(MARGIN, MARGIN, WINDOW_WIDTH - 2 * MARGIN, BUTTON_HEIGHT);
    int x = MARGIN;
    back_ = new Fl_Button(x, MARGIN, BUTTON_WIDTH, BUTTON_HEIGHT, "@<- Back");
    back_->tooltip("The page before (Alt+Left)");
    back_->shortcut(FL_ALT + FL_Left);
    back_->callback([](Fl_Widget *, void *data)
                    {
        auto *self = static_cast<HelpWindow *>(data);
        self->go(self->back_places_, self->forward_places_); }, this);
    x += BUTTON_WIDTH + MARGIN;
    forward_ = new Fl_Button(x, MARGIN, BUTTON_WIDTH, BUTTON_HEIGHT, "Forward @->");
    forward_->tooltip("The page after (Alt+Right)");
    forward_->shortcut(FL_ALT + FL_Right);
    forward_->callback([](Fl_Widget *, void *data)
                       {
        auto *self = static_cast<HelpWindow *>(data);
        self->go(self->forward_places_, self->back_places_); }, this);
    x += BUTTON_WIDTH + MARGIN;
    auto *contents = new Fl_Button(x, MARGIN, BUTTON_WIDTH, BUTTON_HEIGHT, "Contents");
    contents->tooltip("The table of contents");
    contents->callback([](Fl_Widget *, void *data) { static_cast<HelpWindow *>(data)->showContents(); }, this);
    x += BUTTON_WIDTH + MARGIN;
    const int language_x = WINDOW_WIDTH - MARGIN - LANGUAGE_WIDTH;
    auto *spacer = new Fl_Group(x, MARGIN, language_x - x, BUTTON_HEIGHT);
    spacer->end();
    language_ = new Fl_Choice(language_x, MARGIN, LANGUAGE_WIDTH, BUTTON_HEIGHT);
    language_->tooltip("The language of the manual");
    for (const Language &language : LANGUAGES)
        language_->add(language.label);
    language_->value(systemPrefersJapanese() ? 1 : 0);
    language_->callback([](Fl_Widget *, void *data) { static_cast<HelpWindow *>(data)->switchLanguage(); }, this);
    bar->resizable(spacer);
    bar->end();

    const int view_y = MARGIN + BUTTON_HEIGHT + MARGIN;
    view_ = new Fl_Help_View(0, view_y, WINDOW_WIDTH, WINDOW_HEIGHT - view_y);
    view_->textfont(FL_HELVETICA);
    view_->textsize(FONT_SIZE);
    view_->user_data(this);
    view_->link(linkCallback);
    // Called after each load and scroll.
    view_->callback([](Fl_Widget *, void *data) { static_cast<HelpWindow *>(data)->updateWindow(); }, this);

    window_->resizable(view_);
    window_->size_range(400, 300);
    window_->end();
}

HelpWindow::~HelpWindow()
{
    window_.reset();
    if (!directory_.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
}

bool HelpWindow::writePages()
{
    std::vector<HelpFile> sources;
    sources.reserve(EMBEDDED_HELP_FILE_COUNT);
    for (size_t i = 0; i < EMBEDDED_HELP_FILE_COUNT; ++i)
    {
        const EmbeddedHelpFile &file = EMBEDDED_HELP_FILES[i];
        sources.push_back({file.path, std::string(reinterpret_cast<const char *>(file.data), file.size)});
    }
    const std::vector<HelpFile> site = buildHelpSite(sources, MAX_IMAGE_WIDTH);

    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        spdlog::error("No temporary directory for the manual: {}", ec.message());
        return false;
    }
    std::random_device random;
    for (int attempt = 0; attempt < 10 && directory_.empty(); ++attempt)
    {
        const std::filesystem::path candidate = temp / ("tableskim-help-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate, ec))
            directory_ = candidate;
    }
    if (directory_.empty())
    {
        spdlog::error("Cannot create a directory for the manual in {}", viewPath(temp));
        return false;
    }
    for (const HelpFile &file : site)
    {
        const std::filesystem::path path = directory_ / std::filesystem::path(file.path);
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
        if (!out)
        {
            spdlog::error("Cannot write {}", viewPath(path));
            // show() writes the pages only while there is no directory: the
            // next attempt starts over instead of showing missing pages.
            out.close();
            std::filesystem::remove_all(directory_, ec);
            directory_.clear();
            return false;
        }
    }
    return true;
}

std::string HelpWindow::languageCode() const
{
    const int index = language_->value();
    return LANGUAGES[index >= 0 && index < static_cast<int>(std::size(LANGUAGES)) ? index : 0].code;
}

void HelpWindow::show()
{
    // The theme may have changed since the last time.
    view_->color(FL_BACKGROUND2_COLOR);
    view_->textcolor(FL_FOREGROUND_COLOR);
    if (directory_.empty())
    {
        if (!writePages())
        {
            NestedLoopGuard guard;
            fl_alert("The user manual could not be prepared; see the log for details.");
            return;
        }
        showContents();
    }
    window_->show();
}

void HelpWindow::load(const std::string &file, int top)
{
    // Smoother screenshots than the default nearest-neighbor scaling.
    const Fl_RGB_Scaling scaling = Fl_Image::RGB_scaling();
    Fl_Image::RGB_scaling(FL_RGB_SCALING_BILINEAR);
    view_->load(file.c_str());
    Fl_Image::RGB_scaling(scaling);
    if (top > 0)
        view_->topline(top);
    updateWindow();
}

void HelpWindow::go(std::vector<Place> &from, std::vector<Place> &to)
{
    if (from.empty())
        return;
    const Place place = from.back();
    from.pop_back();
    if (const char *current = view_->filename())
        to.push_back({current, view_->topline()});
    going_back_or_forward_ = true;
    load(place.file, place.top);
    going_back_or_forward_ = false;
}

void HelpWindow::showContents()
{
    load(viewPath(directory_ / helpStartPage(languageCode())));
}

// The same page in the other language, if the manual has it.
void HelpWindow::switchLanguage()
{
    std::filesystem::path page = directory_ / helpStartPage(languageCode());
    if (const char *current = view_->filename())
    {
        const std::filesystem::path same = directory_ / languageCode() / std::filesystem::path(current).filename();
        std::error_code ec;
        if (std::filesystem::exists(same, ec))
            page = same;
    }
    load(viewPath(page));
}

void HelpWindow::updateWindow()
{
    const std::string title = view_->title() ? view_->title() : "";
    std::string label = title.empty() ? "TableSkim User Manual" : title;
    if (label.find("TableSkim") == std::string::npos)
        label += " - TableSkim";
    if (label != title_)
    {
        title_ = label;
        window_->copy_label(title_.c_str());
    }
    back_places_.empty() ? back_->deactivate() : back_->activate();
    forward_places_.empty() ? forward_->deactivate() : forward_->activate();
}

// Called before Fl_Help_View loads a page or an image: a page reached by a
// link is remembered for Back. Web links are opened by Fl_Help_View itself.
const char *HelpWindow::linkCallback(Fl_Widget *widget, const char *uri)
{
    auto *self = static_cast<HelpWindow *>(widget->user_data());
    const std::string_view target(uri);
    if (target.starts_with("http:") || target.starts_with("https:"))
        return nullptr; // the browser could not be opened; stay on the page
    if (self && !self->going_back_or_forward_ && target.ends_with(".html"))
    {
        const char *current = self->view_->filename();
        if (current && target != current)
        {
            self->back_places_.push_back({current, self->view_->topline()});
            self->forward_places_.clear();
        }
    }
    return uri;
}

void showHelpWindow()
{
    if (!help_window)
        help_window = std::make_unique<HelpWindow>();
    help_window->show();
}
