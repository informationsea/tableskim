#include "gui_code_export.hxx"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <fstream>
#include <iterator>

#include "spdlog/spdlog.h"

#include "file_util.hxx"
#include "gui_table_view.hxx"
#include "gui_theme.hxx"

namespace
{
    // The folder and language of the last code saved, for the next one.
    std::string &lastDirectory()
    {
        static std::string directory;
        return directory;
    }
    int last_filter = 0; // an index into CODE_LANGUAGES
} // namespace

void saveAnalysisCode(const AnalysisCodeSpec &spec, const std::string &directory)
{
    const CodeLanguage preset_language = CODE_LANGUAGES[last_filter];
    Fl_Native_File_Chooser chooser;
    chooser.title("Save Code");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    // In the order of CODE_LANGUAGES.
    chooser.filter("R, tidyverse\t*.R\nPython, polars + plotnine\t*.py\nPython, pandas + matplotlib\t*.py");
    chooser.filter_value(last_filter);
    const std::string start = lastDirectory().empty() ? directory : lastDirectory();
    if (!start.empty())
        chooser.directory(start.c_str());
    const std::string preset = analysisCodeFileName(spec, preset_language);
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
    // The filter chosen tells the language; a name the chooser did not
    // change still says the language of the filter shown first.
    const int filter = chooser.filter_value();
    CodeLanguage language = preset_language;
    if (filter >= 0 && filter < static_cast<int>(std::size(CODE_LANGUAGES)) && filter != last_filter)
        language = CODE_LANGUAGES[filter];
    else if (const auto named = codeLanguageFromFileName(target))
        language = *named;
    // A name suggested for another language would say the wrong one
    // ("data-view.R.py", polars code in "data-view-pandas.py").
    const std::filesystem::path chosen_target = target;
    target = analysisCodeTarget(spec, chosen_target, language);
    if (target != chosen_target)
    {
        // Not checked by SAVEAS_CONFIRM, which saw the name chosen.
        std::error_code exists_error;
        if (std::filesystem::exists(target, exists_error))
        {
            NestedLoopGuard guard;
            const std::string name = escapeLabel(utf8String(target.filename()));
            if (fl_choice("%s already exists. Replace it?", "Cancel", "Replace", nullptr, name.c_str()) != 1)
                return;
        }
    }
    last_filter = static_cast<int>(std::find(std::begin(CODE_LANGUAGES), std::end(CODE_LANGUAGES), language) -
                                   std::begin(CODE_LANGUAGES));
    lastDirectory() = utf8String(target.parent_path());

    std::ofstream out(target, std::ios::binary);
    out << analysisCode(spec, language);
    out.close();
    if (!out)
    {
        const std::string error = "Cannot write " + utf8String(target);
        spdlog::error("{}", error);
        NestedLoopGuard guard;
        fl_alert("%s", escapeLabel(error).c_str());
    }
}
