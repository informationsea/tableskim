#include "tui_app.hxx"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string_view>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "spdlog/spdlog.h"

#include "cell_format.hxx"
#include "file_util.hxx"
#include "licenses.hxx"
#include "sql_text.hxx"
#include "tableskim_version.hxx"
#include "tui_search.hxx"
#include "tui_text.hxx"

using namespace ftxui;

namespace
{
    // Auto Revert polls instead of watching the file, which keeps it portable
    // and works on network drives.
    const auto AUTO_REVERT_INTERVAL = std::chrono::seconds(1);
    const int COLUMN_WIDTH_STEP = 2;
    const int MAX_SEARCH_STATUS_WIDTH = 16;
    const int MAX_SORT_STATUS_WIDTH = 16;
    const int MAX_FILTER_STATUS_WIDTH = 24;
    const int MAX_FILTER_DIALOG_WIDTH = 76;
    // Match the GUI's limit on memory used by a single clear and its undo.
    const size_t MAX_CLEARED_CELLS = 5000000;
    const int FILTER_OP_WIDTH = 12;

    // Adds the column to the list in column order, or takes it out.
    void toggleColumn(std::vector<size_t> &columns, size_t column)
    {
        const auto it = std::lower_bound(columns.begin(), columns.end(), column);
        if (it != columns.end() && *it == column)
            columns.erase(it);
        else
            columns.insert(it, column);
    }

    const char *const KEY_HELP[][2] = {
        {"Arrows, h j k l", "Move the cursor"},
        {"Shift+Arrows", "Extend the selection"},
        {"v", "Start / stop selecting with cursor moves"},
        {"Ctrl+A", "Select all"},
        {"Esc", "Clear the selection (while exporting or converting: cancel it)"},
        {"PgUp / PgDn, Ctrl+B / Ctrl+F", "Move one page"},
        {"g / G, Ctrl+Up / Ctrl+Down", "First / last row"},
        {"Ctrl+G", "Go to a row (120), a cell (B12, R12C2) or a column (name or letter)"},
        {"Home / End, 0 / $", "First / last column"},
        {"Enter, double click", "Show the whole cell in the sidebar (in edit mode: edit it)"},
        {"S", "Statistics of the cursor's column in the sidebar (again: the cell)"},
        {"B", "Histogram of the cursor's column (Enter: filter to a bar; Esc: back)"},
        {"M", "2D density of the selected two columns, else of number or date columns near the cursor"},
        {"A", "Group By: rows per value of columns, with sums, means, ..., in a sheet"},
        {"%", "Format the column(s): thousands separator, decimal places, date format"},
        {"< / >", "Narrow / widen the column"},
        {"x / X", "Hide the column (or the selected ones) / show all columns"},
        {"|", "Freeze the columns up to the cursor's (again: unfreeze)"},
        {"o", "Sort by the column: ascending, descending, none"},
        {"Tab / Shift+Tab, ] / [", "Next / previous sheet"},
        {"/", "Search the sheet (empty: clear the search)"},
        {"Ctrl+R (in the / prompt)", "Toggle regular expression search"},
        {"n / N", "Next / previous search hit"},
        {"f", "Edit the filter (conditions on columns)"},
        {"= / !", "Filter: equal / not equal to the cell"},
        {"F", "Clear the filter"},
        {"y, c", "Copy the selection (tab-separated)"},
        {"s", "Show / hide the sidebar"},
        {"H", "First row: switch between the guess and header / data (CSV / TSV / Excel)"},
        {"#", "Skip lines starting with ## (on by default for VCF)"},
        {"E", "Next text encoding: UTF-8, UTF-16LE, UTF-16BE, Shift_JIS (CP932)"},
        {":", "Run SQL (DuckDB / SQLite / Parquet / JSON); a SELECT is shown as a sheet"},
        {"Up / Down, Tab (in the : prompt)", "SQL run before; complete a table, column or keyword"},
        {"w", "Export the sheet as shown (sorted, filtered, without hidden columns) to CSV / TSV / Excel / Parquet"},
        {"P / D", "Convert to Parquet / DuckDB next to the file and open it (Esc: cancel)"},
        {"r", "Revert (reload the file)"},
        {"R", "Toggle Auto Revert"},
        {"T", "Follow the end of the file as it grows, like tail -f (again: stop)"},
        {"e", "Show the file in the file browser"},
        {"I", "Edit mode on / off (CSV / TSV / Excel); cells can be changed only in it"},
        {"Enter, i, F2 (edit mode)", "Edit the cell (Enter: done, Tab: done and next column, Esc: cancel)"},
        {"Delete, Backspace (edit mode)", "Clear the selected cells"},
        {"Paste (edit mode)", "Paste tab-separated text into the selected cells"},
        {"u / Ctrl+R (edit mode)", "Undo / redo"},
        {"Ctrl+S (edit mode)", "Save the file"},
        {"?, F1", "This help"},
        {"q, Ctrl+C", "Quit (asks about unsaved changes; Ctrl+C twice: quit without saving)"},
    };

    std::vector<std::string> splitLines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::string line;
        std::istringstream stream(text);
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // Tabs have no fixed width in a terminal cell grid.
            std::replace(line.begin(), line.end(), '\t', ' ');
            lines.push_back(std::move(line));
        }
        if (lines.empty())
            lines.emplace_back();
        return lines;
    }

    // The SQL prompt is one line: a line break shows as ↵.
    std::string promptLine(std::string_view text)
    {
        std::string line;
        line.reserve(text.size());
        for (const char c : text)
        {
            if (c == '\n')
                line += "↵";
            else if (c == '\t')
                line += ' ';
            else if (c != '\r')
                line += c;
        }
        return line;
    }

    // R1C1 notation: columns are shown as numbers, not letters.
    std::string cellName(size_t row, size_t col)
    {
        return "R" + std::to_string(row + 1) + "C" + std::to_string(col + 1);
    }

    // The next column of `count` after `column` (before it if !forward) that
    // is not hidden, going round if `wrap`; none if there is no other.
    std::optional<size_t> nextVisibleColumn(const HiddenColumns &hidden, size_t column, bool forward, size_t count,
                                            bool wrap)
    {
        for (size_t step = 1; step < count; ++step)
        {
            if (!wrap && (forward ? column + step >= count : step > column))
                break;
            const size_t c = forward ? (column + step) % count : (column + count - step % count) % count;
            if (!hidden.contains(c))
                return c;
        }
        return std::nullopt;
    }
} // namespace

TuiApp::TuiApp(App &app) : app_(app)
{
    table_holder_ = Container::Vertical({});

    Component text_dialog = Renderer([this](bool)
                                     { return renderTextDialog(); });
    text_dialog = CatchEvent(text_dialog, [this](Event event)
                             { return handleTextDialogEvent(event); });

    Component main = Renderer(table_holder_, [this]
                              { return renderMain(); });
    main = CatchEvent(main, [this](Event event)
                      { return handleEvent(event); });
    Component filter_dialog = Renderer([this](bool)
                                       { return renderFilterDialog(); });
    filter_dialog = CatchEvent(filter_dialog, [this](Event event)
                               { return handleFilterDialogEvent(event); });
    Component group_dialog = Renderer([this](bool)
                                      { return renderGroupDialog(); });
    group_dialog = CatchEvent(group_dialog, [this](Event event)
                              { return handleGroupDialogEvent(event); });
    Component format_dialog = Renderer([this](bool)
                                       { return renderFormatDialog(); });
    format_dialog = CatchEvent(format_dialog, [this](Event event)
                               { return handleFormatDialogEvent(event); });

    // The text dialog (e.g. an error) goes over the others.
    Component dialogs = main | Modal(filter_dialog, &show_filter_dialog_) | Modal(group_dialog, &show_group_dialog_) |
                        Modal(format_dialog, &show_format_dialog_) | Modal(text_dialog, &show_text_dialog_);
    root_ = CatchEvent(dialogs, [this, dialogs](Event event)
                       {
        // Quit through the destructor, which stops an export or conversion
        // (removing its partial file); FTXUI would raise SIGINT instead.
        if (event == Event::CtrlC)
        {
            // Twice: quit without saving.
            if (book_ && book_->modified() && !unsaved_action_)
                checkUnsaved([this] { app_.Exit(); });
            else
                app_.Exit();
            return true;
        }
        return handlePasteEvent(event, dialogs); });
    app_.ForceHandleCtrlC(false);

    auto_revert_thread_ = std::jthread([this](std::stop_token stop)
                                       {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!wake.wait_for(lock, stop, AUTO_REVERT_INTERVAL, [] { return false; }))
        {
            if (stop.stop_requested())
                break;
            app_.Post([this] { revertIfChanged(); });
        } });
}

TuiApp::~TuiApp()
{
    convert_thread_ = {};
    auto_revert_thread_ = {};
    // The views unsubscribe before the book stops its worker.
    table_holder_->DetachAllChildren();
    views_.clear();
    book_.reset();
}

TuiTableView::Poster TuiApp::poster()
{
    return [app = &app_](std::function<void()> task)
    {
        app->Post(std::move(task));
        // A posted closure does not redraw by itself.
        app->PostEvent(Event::Custom);
    };
}

TuiTableView *TuiApp::currentView() const
{
    return sheet_ < views_.size() ? views_[sheet_].get() : nullptr;
}

const HiddenColumns &TuiApp::hiddenColumnsOf(const std::shared_ptr<TableModel> &model) const
{
    static const HiddenColumns none;
    for (const auto &view : views_)
    {
        if (view && model && view->model() == model)
            return view->hiddenColumns();
    }
    return none;
}

bool TuiApp::open(const std::string &path, std::string *error)
{
    spdlog::debug("Open: {}", path);
    // Taken before reading so that a change made while reading is not missed.
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    const bool has_stamp = readFileStamp(utf8Path(path), mtime, size);
    std::shared_ptr<TableBook> book;
    try
    {
        // The first row is the header if it looks like one (text only, each
        // value different), unless H or --header says otherwise.
        OpenOptions options;
        options.header = first_row_;
        options.skip_meta_lines = skip_meta_lines_;
        options.encoding = text_encoding_;
        book = TableBook::open(utf8Path(path), options);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to open {}: {}", path, e.what());
        if (error)
            *error = e.what();
        else
            showError("Failed to open " + path + ":\n" + e.what());
        return false;
    }
    if (has_stamp)
    {
        file_mtime_ = mtime;
        file_size_ = size;
    }

    // Reopening the same file stays on the same sheet and position.
    std::string selected_sheet;
    if (path == path_ && (book_ || !closed_sheet_names_.empty()))
    {
        // The book is closed after saving.
        const std::vector<std::string> names = book_ ? book_->sheetNames() : closed_sheet_names_;
        if (sheet_ < names.size())
            selected_sheet = names[sheet_];
        sql_reselect_ = !sql_sheet_.empty() && selected_sheet == sql_sheet_;
        // Sorts and filters are kept too.
        for (size_t i = 0; i < views_.size() && i < names.size(); ++i)
        {
            if (!views_[i])
                continue;
            restore_positions_[names[i]] = views_[i]->scrollPosition();
            if (views_[i]->model())
            {
                restore_views_[names[i]] = views_[i]->model()->requestedView();
                if (!views_[i]->hiddenColumns().empty())
                    restore_hidden_[names[i]] = views_[i]->hiddenColumns();
                if (!views_[i]->columnFormats().empty())
                    restore_formats_[names[i]] = views_[i]->columnFormats();
                if (views_[i]->frozenColumns() > 0)
                    restore_frozen_[names[i]] = views_[i]->frozenColumns();
                if (const ColumnStatsState stats = views_[i]->model()->columnStatsState(); stats.active)
                    restore_stats_[names[i]] = stats.column;
            }
        }
    }
    else
    {
        restore_positions_.clear();
        restore_views_.clear();
        restore_stats_.clear();
        restore_hidden_.clear();
        restore_frozen_.clear();
        restore_formats_.clear();
        last_query_.clear();
        sql_sheet_.clear();
        group_result_.reset();
        group_sheet_.clear();
        group_draft_ = {};
        editing_ = false;
    }
    path_ = path;
    closed_sheet_names_.clear();
    changed_on_disk_ = false;
    edit_prompt_ = false;
    edit_model_.reset();

    // Dropping the views unsubscribes them before the old book stops its worker.
    table_holder_->DetachAllChildren();
    views_.clear();
    // A question about the old file's memory would be answered for nothing,
    // while the new file waits at its own checkpoint without asking.
    memory_prompt_model_.reset();
    book_ = std::move(book);
    // What the old book was asked to read is dropped with it.
    append_pending_ = false;
    // A Group By's rows are kept in memory: the new book shows them again.
    if (group_result_)
        group_sheet_ = book_->addResultSheet(GROUP_BY_SHEET, group_result_);
    // Edit mode stays on when the same file is reopened (e.g. after saving).
    book_->setEditing(editing_);
    editing_ = book_->editing();
    // The old book's completion callback is discarded after a reopen.
    sql_running_ = false;
    views_.resize(book_->sheetNames().size());

    const auto &names = book_->sheetNames();
    const auto it = std::find(names.begin(), names.end(), selected_sheet);
    showSheet(it != names.end() ? static_cast<size_t>(it - names.begin()) : 0);
    if (!last_query_.empty() && book_->supportsSql())
        runSql(last_query_, true);
    return true;
}

// `rerun`: the query of the SQL sheet, run again after the file was reopened.
void TuiApp::runSql(const std::string &sql, bool rerun)
{
    if (!book_ || !book_->supportsSql())
        return;
    if (!rerun)
        sql_running_ = true;
    book_->executeSql(sql, [this, post = poster(), book = std::weak_ptr<TableBook>(book_), sql, rerun](const TableBook::SqlResult &result)
                      { post([this, book, sql, rerun, result]
                             {
            if (auto locked = book.lock())
                sqlFinished(locked, sql, rerun, result); }); });
}

void TuiApp::sqlFinished(const std::shared_ptr<TableBook> &book, const std::string &sql, bool rerun,
                         const TableBook::SqlResult &result)
{
    // A result for a book that was replaced meanwhile (revert) is stale.
    if (book != book_)
        return;
    if (!rerun)
        sql_running_ = false;
    loadSqlSchema(); // the SQL may have created tables
    if (!result.ok)
    {
        if (rerun)
        {
            // E.g. a table the query used is gone.
            spdlog::warn("Running the query again failed: {}", result.message);
            last_query_.clear();
            return;
        }
        showError("SQL error:\n" + result.message);
        return;
    }
    if (result.sheet.empty())
    {
        setStatus(result.message);
        return;
    }

    last_query_ = sql;
    sql_sheet_ = result.sheet;
    const auto names = book_->sheetNames();
    const size_t index = static_cast<size_t>(std::find(names.begin(), names.end(), result.sheet) - names.begin());
    if (index >= names.size())
        return;
    if (views_.size() + 1 == names.size() && index < views_.size())
    {
        // A new sheet before the result sheets (a Group By): the views, and
        // the sheet shown, move with their names.
        views_.insert(views_.begin() + static_cast<std::ptrdiff_t>(index), nullptr);
        if (sheet_ >= index)
            ++sheet_;
    }
    views_.resize(names.size());
    if (!rerun)
    {
        // New rows: a fresh view, not the old position, sort or filter
        // (kept for the old result's view, or by a reopen before it was shown).
        views_[index].reset();
        restore_positions_.erase(result.sheet);
        restore_views_.erase(result.sheet);
        restore_stats_.erase(result.sheet);
        restore_hidden_.erase(result.sheet);
        restore_frozen_.erase(result.sheet);
        restore_formats_.erase(result.sheet);
    }
    if (!rerun || sql_reselect_)
        showSheet(index);
    else if (index == sheet_)
        showSheet(index); // the view was dropped with the old book
    sql_reselect_ = false;
    if (!rerun)
        setStatus("The result is shown in the sheet \"" + result.sheet + "\"");
}

void TuiApp::showSheet(size_t index)
{
    if (!book_ || index >= views_.size())
        return;
    sheet_ = index;
    const std::string name = book_->sheetNames()[index];
    if (!views_[index])
    {
        auto view = Make<TuiTableView>(book_->sheet(name), poster());
        view->setActivateCallback([this]
                                  {
            if (editing())
            {
                editCell();
                return;
            }
            show_sidebar_ = true;
            sidebar_mode_ = SidebarMode::Cell; });
        view->setErrorCallback([this, name](const std::string &message)
                               { showError("Failed to load " + name + ":\n" + message); });
        view->setUpdateCallback([this, raw = view.get()]
                                {
            checkMemoryCheckpoint(raw->model());
            // The sheet was reopened or replaced: its new model's histogram.
            if (histogram_model_ && currentView() == raw && raw->model() && raw->model() != histogram_model_)
                showHistogram(histogram_model_->histogramState().column);
            if (density_model_ && currentView() == raw && raw->model() && raw->model() != density_model_)
            {
                const ScatterState state = density_model_->scatterState();
                showDensity(state.x_column, state.y_column);
            }
            if (currentView() == raw)
                refreshSearch(); });
        if (auto it = restore_positions_.find(name); it != restore_positions_.end())
        {
            view->restoreScrollPosition(it->second);
            restore_positions_.erase(it);
        }
        if (auto it = restore_views_.find(name); it != restore_views_.end())
        {
            view->restoreView(std::move(it->second));
            restore_views_.erase(it);
        }
        if (auto it = restore_hidden_.find(name); it != restore_hidden_.end())
        {
            view->restoreHiddenColumns(it->second);
            restore_hidden_.erase(it);
        }
        if (auto it = restore_frozen_.find(name); it != restore_frozen_.end())
        {
            view->freezeColumns(it->second);
            restore_frozen_.erase(it);
        }
        if (auto it = restore_formats_.find(name); it != restore_formats_.end())
        {
            view->setColumnFormats(it->second);
            restore_formats_.erase(it);
        }
        if (auto it = restore_stats_.find(name); it != restore_stats_.end())
        {
            if (view->model())
                view->model()->startColumnStats(it->second);
            restore_stats_.erase(it);
        }
        view->setFollowEnd(follow_);
        views_[index] = std::move(view);
    }
    table_holder_->DetachAllChildren();
    table_holder_->Add(views_[index]);
    syncSearch();
}

void TuiApp::revert()
{
    if (path_.empty())
        return;
    // Copy: open() assigns path_.
    const std::string path = path_;
    if (open(path))
        setStatus("Reverted");
}

// Reopening while exporting would close the sheet being written (as Auto
// Revert waits too); a conversion would then open its result over it.
bool TuiApp::refuseReopen()
{
    if (saving_)
    {
        setStatus("Wait until the file is saved", true);
        return true;
    }
    if (!converting_)
        return false;
    setStatus("Wait until " + convert_label_ + " has finished (Esc cancels it)", true);
    return true;
}

void TuiApp::setFollow(bool enabled)
{
    follow_ = enabled;
    for (const auto &view : views_)
    {
        if (view)
            view->setFollowEnd(enabled);
    }
    setStatus(enabled ? "Following the end of the file" : "Not following the file");
    if (enabled)
        revertIfChanged();
}

void TuiApp::revertIfChanged()
{
    // An export reads the open file to the end; a reopen would stop it.
    if ((!auto_revert_ && !follow_) || path_.empty() || converting_)
        return;
    // Following: only the appended rows are read, if the file allows. Edit
    // mode reads the file again as Auto Revert does, which keeps the edits.
    if (follow_ && book_ && book_->readsAppendedRows() && !editing())
    {
        if (append_pending_)
            return;
        append_pending_ = true;
        book_->readAppended([this, post = poster(), book = std::weak_ptr<TableBook>(book_)](AppendResult result, bool caught_up)
                            { post([this, book, result, caught_up]
                                   {
            // A reopen meanwhile: the new book has read the file anew.
            if (book.lock() != book_)
                return;
            append_pending_ = false;
            // All read: no reopen when Follow is turned off. With bytes
            // left (a row being written), the old stamp makes Auto Revert
            // read them.
            if (result != AppendResult::Changed && caught_up)
                readFileStamp(utf8Path(path_), file_mtime_, file_size_);
            else if (result == AppendResult::Changed)
            {
                std::filesystem::file_time_type mtime;
                std::uintmax_t size;
                // Being replaced: tried again on the next poll.
                if (!readFileStamp(utf8Path(path_), mtime, size) || size == 0)
                    return;
                spdlog::debug("Follow: {} was changed, reading it again", path_);
                file_mtime_ = mtime;
                file_size_ = size;
                const std::string path = path_;
                std::string error;
                open(path, &error);
            } }); });
        return;
    }
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    // A missing or empty file is usually being replaced or rewritten; wait
    // until it has content again.
    if (!readFileStamp(utf8Path(path_), mtime, size) || size == 0 || (mtime == file_mtime_ && size == file_size_))
        return;
    if (saving_ || (book_ && book_->modified()))
    {
        // Reopening would lose the edits; the status bar tells, and saving fails.
        if (!saving_ && !changed_on_disk_)
        {
            changed_on_disk_ = true;
            app_.PostEvent(Event::Custom);
        }
        return;
    }
    spdlog::debug("Auto revert: {} changed", path_);
    // Remember the new stamp even if reading fails (e.g. a half-written file),
    // so that a broken file is not retried on every poll.
    file_mtime_ = mtime;
    file_size_ = size;
    const std::string path = path_;
    std::string error;
    open(path, &error);
    app_.PostEvent(Event::Custom);
}

bool TuiApp::headerOptionAvailable() const
{
    // Formats with their own column names (Parquet, databases) ignore it.
    return !book_ || book_->usesHeaderOption();
}

void TuiApp::setFirstRow(OpenOptions::Header first_row)
{
    if (!headerOptionAvailable())
    {
        setStatus("This file has its own column names", true);
        return;
    }
    const OpenOptions::Header old = first_row_;
    first_row_ = first_row;
    if (path_.empty())
        return;
    const std::string path = path_;
    if (!open(path))
    {
        // The shown file still uses the old setting (open() reported why).
        first_row_ = old;
        return;
    }
    // The sheet is read after this, so the guess is not known yet.
    switch (first_row_)
    {
    case OpenOptions::Header::Auto:
        setStatus("First row: header if it looks like one (H: switch)");
        break;
    case OpenOptions::Header::Yes:
        setStatus("First row is the header (H: guess again)");
        break;
    case OpenOptions::Header::No:
        setStatus("First row is data (H: guess again)");
        break;
    }
}

// Switches between the guess and the other choice, judged by the sheet
// shown: whether its first row is the header.
void TuiApp::toggleFirstRow()
{
    if (first_row_ != OpenOptions::Header::Auto)
    {
        setFirstRow(OpenOptions::Header::Auto);
        return;
    }
    const TuiTableView *view = currentView();
    const bool header = view && view->model() && view->model()->hasHeader();
    setFirstRow(header ? OpenOptions::Header::No : OpenOptions::Header::Yes);
}

bool TuiApp::skipMetaLinesAvailable() const
{
    return !book_ || book_->isTextFormat();
}

bool TuiApp::skipsMetaLines() const
{
    return book_ ? book_->skipsMetaLines() : skip_meta_lines_.value_or(false);
}

void TuiApp::setSkipMetaLines(bool enabled)
{
    if (!skipMetaLinesAvailable())
    {
        setStatus("Only text files can skip lines", true);
        return;
    }
    const std::optional<bool> old = skip_meta_lines_;
    skip_meta_lines_ = enabled;
    if (!path_.empty())
    {
        const std::string path = path_;
        if (!open(path))
        {
            skip_meta_lines_ = old;
            return;
        }
    }
    setStatus(enabled ? "Skipping lines starting with ##" : "Showing lines starting with ##");
}

TextEncoding TuiApp::textEncoding() const
{
    if (book_ && book_->textEncoding())
        return *book_->textEncoding();
    return text_encoding_.value_or(TextEncoding::Utf8);
}

void TuiApp::setTextEncoding(TextEncoding encoding)
{
    if (!skipMetaLinesAvailable())
    {
        setStatus("Only text files have an encoding", true);
        return;
    }
    const std::optional<TextEncoding> old = text_encoding_;
    text_encoding_ = encoding;
    if (!path_.empty())
    {
        const std::string path = path_;
        if (!open(path))
        {
            text_encoding_ = old;
            return;
        }
    }
    setStatus(std::string("Encoding: ") + textEncodingName(encoding));
}

// There is no file dialog: the result goes next to the file (data.csv.gz ->
// data.parquet), after asking before an existing file is replaced.
void TuiApp::convertTo(ExportFormat format)
{
    if (converting_)
        return;
    if (!book_ || !book_->isTextFormat())
    {
        setStatus(std::string("Only CSV / TSV files can be converted to ") + exportFormatName(format), true);
        return;
    }
    convert_format_ = format;
    const std::filesystem::path target = defaultExportPath(utf8Path(path_), format);
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        confirm_overwrite_ = Overwrite::Convert;
        setStatus(utf8String(target.filename()) + " exists. Overwrite? (y/n)");
        return;
    }
    startConversion();
}

void TuiApp::startConversion()
{
    const std::filesystem::path source = utf8Path(path_);
    const ExportFormat format = convert_format_;
    const std::filesystem::path target = defaultExportPath(source, format);
    if (book_->modified())
        setStatus("Converting the saved file: the unsaved changes are not converted");
    // Convert what is shown: the same header, ## and encoding choices.
    OpenOptions options;
    options.header = first_row_;
    options.skip_meta_lines = book_->skipsMetaLines();
    options.encoding = book_->textEncoding();
    converting_ = true;
    convert_label_ = "converting";
    convert_rows_ = 0;
    convert_thread_ = std::jthread([this, source, target, format, options, post = poster()](std::stop_token stop)
                                   {
        std::optional<size_t> rows;
        std::string error;
        try
        {
            rows = exportTextFile(source, target, format, options, stop, [this, &post](size_t n)
                                  { post([this, n] { convert_rows_ = n; }); });
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        post([this, target, format, rows, error]
             {
            converting_ = false;
            if (!error.empty())
            {
                const std::string message = std::string("Failed to convert to ") + exportFormatName(format) + ":\n" + error;
                spdlog::error("{}", message);
                showError(message);
            }
            else if (!rows)
                setStatus("Conversion canceled");
            else if (saving_ || (book_ && book_->modified()))
            {
                // Opening the result would close the file with the edits.
                setStatus("Converted " + std::to_string(*rows) + " rows to " + utf8String(target.filename()) +
                          " (not opened: there are unsaved changes)");
            }
            else if (open(utf8String(target)))
                setStatus("Converted " + std::to_string(*rows) + " rows to " + utf8String(target.filename()));
        }); });
}

void TuiApp::openExportPrompt()
{
    TuiTableView *view = currentView();
    if (converting_ || !view || !view->model())
        return;
    const std::string sheet = book_->sheetNames().size() > 1 ? view->model()->name() : std::string();
    // The format of the last export.
    const ViewExportFormat format = viewExportFormatFromPath(export_target_).value_or(ViewExportFormat::Csv);
    export_input_ = utf8String(defaultViewExportPath(utf8Path(path_), sheet, format));
    export_model_ = view->model();
    export_prompt_ = true;
}

bool TuiApp::handleExportPromptEvent(Event event)
{
    if (event == Event::Return)
    {
        export_prompt_ = false;
        exportTo(export_input_);
    }
    else if (event == Event::Escape)
    {
        export_prompt_ = false;
        export_model_.reset();
    }
    else if (event == Event::Backspace)
        eraseLastCharacter(export_input_);
    else if (event == Event::CtrlU)
        export_input_.clear();
    else if (event.is_character())
        export_input_ += event.character();
    return true;
}

// The format follows the extension; an existing file is replaced only after
// asking.
void TuiApp::exportTo(const std::string &text)
{
    const size_t begin = text.find_first_not_of(' ');
    if (begin == std::string::npos)
    {
        export_model_.reset();
        return;
    }
    const std::filesystem::path target = utf8Path(text.substr(begin, text.find_last_not_of(' ') - begin + 1));
    if (!viewExportFormatFromPath(target))
    {
        export_model_.reset();
        setStatus("Choose a name ending in .csv, .tsv, .xlsx or .parquet", true);
        return;
    }
    export_target_ = target;
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        confirm_overwrite_ = Overwrite::Export;
        setStatus(utf8String(target.filename()) + " exists. Overwrite? (y/n)");
        return;
    }
    startExport();
}

void TuiApp::startExport()
{
    TuiTableView *view = currentView();
    const std::shared_ptr<TableModel> model = std::exchange(export_model_, nullptr);
    if (converting_ || !view || !view->model())
        return;
    if (view->model() != model)
    {
        setStatus("Not exported: the sheet changed", true);
        return;
    }
    const std::filesystem::path target = export_target_;
    const ViewExportFormat format = *viewExportFormatFromPath(target);
    ViewExportOptions options;
    options.hidden = view->hiddenColumns();
    options.source = utf8Path(path_);
    converting_ = true;
    convert_label_ = "exporting";
    convert_rows_ = 0;
    convert_thread_ = std::jthread([this, model, target, format, options, post = poster()](std::stop_token stop)
                                   {
        std::optional<size_t> rows;
        std::string error;
        try
        {
            rows = exportView(model, target, format, options, stop, [this, &post](size_t n)
                              { post([this, n] { convert_rows_ = n; }); });
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        post([this, target, format, rows, error]
             {
            converting_ = false;
            if (!error.empty())
            {
                const std::string message = std::string("Failed to export the view as ") + viewExportFormatName(format) +
                                            ":\n" + error;
                spdlog::error("{}", message);
                showError(message);
            }
            else if (!rows)
                setStatus("Export canceled");
            else
                setStatus("Exported " + std::to_string(*rows) + " rows to " + utf8String(target.filename()));
        }); });
}

void TuiApp::copySelection()
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return;
    const TuiTableView::Selection sel = view->selection();
    const TsvCopyResult result =
        rangeToTsv(*view->model(), sel.row_top, sel.col_left, sel.row_bottom, sel.col_right, &view->hiddenColumns());
    if (result.copied_rows > 0)
        copyToClipboard(result.text);
    if (result.skipped_rows > 0)
        showTextDialog("Copy", "Some selected data is not available yet.\nCopied " +
                                  std::to_string(result.copied_rows) + " row(s); skipped " +
                                  std::to_string(result.skipped_rows) + " row(s) containing unavailable cells." +
                                  (result.copied_rows == 0 ? "\nThe clipboard was not changed." : ""));
    if (result.copied_rows == 0)
        return;
    const size_t rows = result.copied_rows;
    // Hidden columns are not copied.
    const size_t cols = view->hiddenColumns().visibleIn(sel.col_left, sel.col_right);
    if (rows == 1 && cols == 1 && result.skipped_rows == 0)
        setStatus("Copied " + cellName(sel.row_top, sel.col_left));
    else
        setStatus("Copied " + std::to_string(rows) + " × " + std::to_string(cols) + " cells");
}

void TuiApp::showHelp()
{
    std::string text = "TableSkim " TABLESKIM_VERSION_FULL "\n\n";
    size_t key_width = 0;
    for (const auto &entry : KEY_HELP)
        key_width = std::max(key_width, std::string_view(entry[0]).size());
    for (const auto &entry : KEY_HELP)
        text += "  " + fitText(entry[0], static_cast<int>(key_width), TextAlign::Left) + "  " + entry[1] + "\n";
    text += "\n\n";
    text += thirdPartyLicenses();
    showTextDialog("Help", text, false);
}

void TuiApp::showTextDialog(std::string title, const std::string &text, bool wrap)
{
    dialog_title_ = std::move(title);
    dialog_lines_ = splitLines(text);
    dialog_wrap_ = wrap;
    dialog_rows_width_ = -1;
    dialog_scroll_ = 0;
    show_text_dialog_ = true;
}

void TuiApp::showError(const std::string &message)
{
    showTextDialog("Error", message);
}

void TuiApp::setStatus(std::string message, bool error)
{
    status_message_ = std::move(message);
    status_is_error_ = error;
}

void TuiApp::startSearch(std::string query)
{
    search_spec_ = {.text = std::move(query), .regex = search_regex_};
    pending_jump_ = SearchJump::None;
    TuiTableView *current = currentView();
    for (const auto &view : views_)
    {
        if (view && view.get() != current && view->model())
            view->model()->cancelSearch();
    }
    search_ = {};
    if (!current || !current->model())
        return;
    // startSearch() with empty text cancels the search.
    current->model()->startSearch(search_spec_);
    search_ = current->model()->searchState();
    if (!search_.error.empty())
        setStatus(search_.error, true);
    else if (search_.active)
        jumpToHit(true, true);
}

// Called when another sheet is shown: its search may be missing or stale.
void TuiApp::syncSearch()
{
    pending_jump_ = SearchJump::None;
    search_ = {};
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    if (!search_spec_.text.empty() && !view->model()->searchState().active)
        view->model()->startSearch(search_spec_);
    search_ = view->model()->searchState();
}

void TuiApp::refreshSearch()
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    search_ = view->model()->searchState();
    if (pending_jump_ != SearchJump::None)
        jumpToHit(pending_jump_ == SearchJump::Next, pending_jump_inclusive_);
}

void TuiApp::jumpToHit(bool forward, bool include_from)
{
    pending_jump_ = SearchJump::None;
    TuiTableView *view = currentView();
    if (!view)
        return;
    if (!search_.active)
    {
        setStatus("No search; press / to search", true);
        return;
    }
    const SearchHitLookup found = findSearchHit(search_, {view->cursorRow(), view->cursorColumn()}, forward, include_from);
    if (found.pending)
    {
        // Retried by refreshSearch() as the search goes on.
        pending_jump_ = forward ? SearchJump::Next : SearchJump::Previous;
        pending_jump_inclusive_ = include_from;
        return;
    }
    if (!found.index)
    {
        setStatus(!search_.error.empty() ? search_.error : "Not found: " + search_spec_.text, true);
        return;
    }
    const CellPosition hit = search_.hits[*found.index];
    view->jumpTo(hit.row, hit.col);
    if (found.wrapped)
        setStatus(forward ? "Search wrapped to the top" : "Search wrapped to the bottom");
}

// "/query 3/557": the hit under the cursor and the number of hits; "+" while
// more may be found. Regex searches start with "regex /".
std::string TuiApp::searchStatusText() const
{
    if (!search_.active)
        return {};
    std::string query = search_spec_.text;
    if (string_width(query) > MAX_SEARCH_STATUS_WIDTH)
        query = fitText(query, MAX_SEARCH_STATUS_WIDTH, TextAlign::Left);
    query = (search_spec_.regex ? "regex /" : "/") + query;
    if (!search_.error.empty())
        return query + " invalid";
    const size_t total = search_.hits.size();
    if (search_.done && total == 0)
        return query + " no hits";
    std::optional<size_t> position;
    if (TuiTableView *view = currentView())
        position = searchHitIndex(search_, {view->cursorRow(), view->cursorColumn()});
    return query + " " + (position ? std::to_string(*position + 1) : "-") + "/" + std::to_string(total) +
           (!search_.done || search_.truncated ? "+" : "");
}

bool TuiApp::filterAvailable()
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return false;
    if (!view->model()->viewCapabilities().filter)
    {
        setStatus("Still opening (cannot filter yet)", true);
        return false;
    }
    return true;
}

void TuiApp::openFilterDialog()
{
    if (!filterAvailable())
        return;
    TuiTableView *view = currentView();
    filter_draft_ = view->model()->requestedView().filter.value_or(Filter{});
    filter_field_ = FilterField::Value;
    const auto &conditions = filter_draft_.conditions;
    if (conditions.empty())
    {
        addFilterDraftCondition();
    }
    else
    {
        // Start at a condition on the cursor column, if there is one.
        const auto it = std::find_if(conditions.begin(), conditions.end(), [&](const FilterCondition &c)
                                     { return c.column == view->cursorColumn(); });
        filter_line_ = static_cast<size_t>((it != conditions.end() ? it : conditions.begin()) - conditions.begin()) + 1;
    }
    show_filter_dialog_ = true;
}

void TuiApp::addFilterDraftCondition()
{
    TuiTableView *view = currentView();
    filter_draft_.conditions.push_back({view ? view->cursorColumn() : 0, FilterOp::Contains, {}});
    filter_line_ = filter_draft_.conditions.size();
    filter_field_ = FilterField::Value;
}

bool TuiApp::applyFilterDraft()
{
    Filter filter = filter_draft_;
    for (FilterCondition &condition : filter.conditions)
    {
        if (!filterOpTakesValue(condition.op))
            condition.value.clear();
    }
    return setFilter(filter.conditions.empty() ? std::nullopt : std::optional(std::move(filter)));
}

void TuiApp::quickFilter(bool equal)
{
    if (!filterAvailable())
        return;
    TuiTableView *view = currentView();
    const size_t row = view->cursorRow();
    const size_t col = view->cursorColumn();
    const CellRef cell = view->model()->peek(row, col);
    CellValue value;
    if (cell.state == CellState::Ready)
        value = *cell.value;
    else if (cell.state != CellState::Empty)
    {
        setStatus("Cell " + cellName(row, col) + " is not ready yet", true);
        return;
    }
    ViewSpec spec = view->model()->requestedView();
    addFilterCondition(spec, cellFilterCondition(col, value, equal));
    setFilter(std::move(spec.filter));
}

void TuiApp::clearFilter()
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    if (!view->model()->requestedView().filtered())
    {
        setStatus("No filter");
        return;
    }
    setFilter(std::nullopt);
}

// False if the filter was refused (an invalid regular expression).
bool TuiApp::setFilter(std::optional<Filter> filter)
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return true;
    const auto &model = view->model();
    if (filter)
    {
        if (const auto error = filterError(*filter))
        {
            setStatus(*error, true);
            return false;
        }
    }
    ViewSpec spec = model->requestedView();
    spec.filter = std::move(filter);
    // The status bar shows the filter itself.
    setStatus(spec.filtered() ? "Filter applied" : "Filter cleared");
    model->setView(std::move(spec));
    return true;
}

std::string TuiApp::filterStatusText() const
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || !view->appliedView().filtered())
        return {};
    std::string summary = singleLine(filterSummary(*view->model(), *view->appliedView().filter));
    if (string_width(summary) > MAX_FILTER_STATUS_WIDTH)
        summary = fitText(summary, MAX_FILTER_STATUS_WIDTH, TextAlign::Left);
    return "filter: " + summary;
}

bool TuiApp::handleFilterDialogEvent(Event event)
{
    if (event == Event::Custom)
        return false;
    if (event.is_mouse())
        return true;

    auto &conditions = filter_draft_.conditions;
    const size_t add_line = conditions.size() + 1;
    FilterCondition *condition =
        filter_line_ >= 1 && filter_line_ <= conditions.size() ? &conditions[filter_line_ - 1] : nullptr;
    TuiTableView *view = currentView();
    const size_t columns = view && view->model() ? view->model()->columnCount() : 0;
    const auto toggle_combine = [&]
    {
        filter_draft_.combine =
            filter_draft_.combine == Filter::Combine::And ? Filter::Combine::Or : Filter::Combine::And;
    };

    if (event == Event::Escape)
        show_filter_dialog_ = false;
    else if (event == Event::Return)
    {
        if (filter_line_ == add_line)
            addFilterDraftCondition();
        else if (applyFilterDraft())
            show_filter_dialog_ = false; // kept open to fix an invalid pattern
    }
    else if (event == Event::ArrowUp)
        filter_line_ = filter_line_ > 0 ? filter_line_ - 1 : 0;
    else if (event == Event::ArrowDown)
        filter_line_ = std::min(filter_line_ + 1, add_line);
    else if (event == Event::Tab || event == Event::TabReverse)
    {
        const int step = event == Event::Tab ? 1 : 2; // +1 or -1 of 3 fields
        if (condition)
            filter_field_ = static_cast<FilterField>((static_cast<int>(filter_field_) + step) % 3);
        else if (event == Event::Tab)
            filter_line_ = std::min(filter_line_ + 1, add_line);
        else
            filter_line_ = filter_line_ > 0 ? filter_line_ - 1 : 0;
    }
    else if (event == Event::CtrlN || event == Event::Insert)
        addFilterDraftCondition();
    else if ((event == Event::CtrlD || event == Event::Delete) && condition)
    {
        conditions.erase(conditions.begin() + static_cast<std::ptrdiff_t>(filter_line_ - 1));
        filter_line_ = std::min(filter_line_, conditions.size() + 1);
    }
    else if (filter_line_ == 0)
    {
        if (event == Event::ArrowLeft || event == Event::ArrowRight || event == Event::Character(' '))
            toggle_combine();
    }
    else if (condition && (event == Event::ArrowLeft || event == Event::ArrowRight) &&
             filter_field_ != FilterField::Value)
    {
        const bool forward = event == Event::ArrowRight;
        if (filter_field_ == FilterField::Column && columns > 0)
        {
            const size_t column = std::min(condition->column, columns - 1);
            condition->column = forward ? (column + 1) % columns : (column + columns - 1) % columns;
        }
        else if (filter_field_ == FilterField::Operator)
        {
            const size_t count = std::size(FILTER_OPS);
            const auto it = std::find(std::begin(FILTER_OPS), std::end(FILTER_OPS), condition->op);
            const size_t index = it != std::end(FILTER_OPS) ? static_cast<size_t>(it - std::begin(FILTER_OPS)) : 0;
            condition->op = FILTER_OPS[forward ? (index + 1) % count : (index + count - 1) % count];
        }
    }
    else if (condition && filterOpTakesValue(condition->op))
    {
        // Typing on another field of the line goes to the value.
        if (event == Event::Backspace && filter_field_ == FilterField::Value)
            eraseLastCharacter(condition->value);
        else if (event == Event::CtrlU)
            condition->value.clear();
        else if (event.is_character())
        {
            filter_field_ = FilterField::Value;
            condition->value += event.character();
        }
    }
    return true;
}

bool TuiApp::handleSearchPromptEvent(Event event)
{
    if (event == Event::Return)
    {
        search_prompt_ = false;
        startSearch(search_input_);
    }
    else if (event == Event::Escape)
        search_prompt_ = false;
    else if (event == Event::Backspace)
    {
        // Like vim, Backspace on an empty prompt cancels it.
        if (search_input_.empty())
            search_prompt_ = false;
        else
            eraseLastCharacter(search_input_);
    }
    else if (event == Event::CtrlU)
        search_input_.clear();
    else if (event == Event::CtrlR)
        search_regex_ = !search_regex_;
    else if (event.is_character())
        search_input_ += event.character();
    return true;
}

bool TuiApp::handleSqlPromptEvent(Event event)
{
    if (event == Event::Tab)
    {
        completeSqlInput();
        return true;
    }
    sql_completion_.reset();
    if (event == Event::Return)
    {
        sql_prompt_ = false;
        sql_history_cursor_.reset();
        if (sql_input_.find_first_not_of(' ') != std::string::npos)
        {
            sql_history_.add(sql_input_);
            runSql(sql_input_, false);
        }
    }
    else if (event == Event::Escape)
    {
        sql_prompt_ = false;
        sql_history_cursor_.reset();
    }
    else if (event == Event::ArrowUp || event == Event::ArrowDown)
    {
        if (!sql_history_cursor_.walking())
            sql_history_.reload(); // the GUI or another terminal may have run SQL
        const auto sql = event == Event::ArrowUp ? sql_history_cursor_.previous(sql_history_.entries(), sql_input_)
                                                 : sql_history_cursor_.next(sql_history_.entries());
        // Line breaks are kept: a "--" comment ends at one.
        if (sql)
            sql_input_ = *sql;
    }
    else if (event == Event::Backspace)
    {
        sql_history_cursor_.reset();
        if (sql_input_.empty())
            sql_prompt_ = false;
        else
            eraseLastCharacter(sql_input_);
    }
    else if (event == Event::CtrlU)
    {
        sql_history_cursor_.reset();
        sql_input_.clear();
    }
    else if (event.is_character())
    {
        sql_history_cursor_.reset();
        sql_input_ += event.character();
    }
    return true;
}

// The first Tab inserts the only candidate, or what the candidates share;
// further ones cycle through them.
void TuiApp::completeSqlInput()
{
    if (sql_completion_ && !sql_completion_->candidates.empty())
    {
        auto &cycle = *sql_completion_;
        sql_input_ = cycle.head + cycle.candidates[cycle.next % cycle.candidates.size()];
        ++cycle.next;
        return;
    }
    const SqlCompletion completion = completeSql(sql_input_, sql_input_.size(), sql_schema_);
    if (completion.candidates.empty())
        return;
    const std::string head = sql_input_.substr(0, completion.begin);
    if (completion.candidates.size() == 1)
    {
        sql_input_ = head + completion.candidates.front();
        return;
    }
    SqlCompletionCycle cycle{head, completion.candidates, 0};
    const std::string common = commonCompletionPrefix(completion.candidates);
    if (common.size() > completion.end - completion.begin)
        sql_input_ = head + common;
    else
    {
        sql_input_ = head + cycle.candidates.front();
        cycle.next = 1;
    }
    sql_completion_ = std::move(cycle);
}

void TuiApp::loadSqlSchema()
{
    if (!book_ || !book_->supportsSql())
        return;
    book_->loadSqlSchema([this, post = poster(), book = std::weak_ptr<TableBook>(book_)](std::vector<SqlTable> schema)
                         { post([this, book, schema = std::move(schema)]
                                {
            if (book.lock() == book_)
                sql_schema_ = schema; }); });
}

bool TuiApp::handleGoToPromptEvent(Event event)
{
    if (event == Event::Return)
    {
        goto_prompt_ = false;
        goTo(goto_input_);
    }
    else if (event == Event::Escape)
        goto_prompt_ = false;
    else if (event == Event::Backspace)
    {
        if (goto_input_.empty())
            goto_prompt_ = false;
        else
            eraseLastCharacter(goto_input_);
    }
    else if (event == Event::CtrlU)
        goto_input_.clear();
    else if (event.is_character())
        goto_input_ += event.character();
    return true;
}

// Reading CSV / TSV keeps every row in memory; each memory checkpoint asks
// whether to go on.
void TuiApp::checkMemoryCheckpoint(const std::shared_ptr<TableModel> &model)
{
    if (!memory_prompt_model_ && model && model->memoryCheckpoint())
    {
        memory_prompt_model_ = model;
        const char *prompt = search_prompt_ ? "search" : goto_prompt_ ? "goto" : export_prompt_ ? "export"
                                                   : sql_prompt_ ? "sql" : "none";
        spdlog::debug("Memory checkpoint: prompt={}", prompt);
    }
}

bool TuiApp::handleMemoryPromptEvent(Event event)
{
    if (event == Event::y || event == Event::Y)
        memory_prompt_model_->continueReading();
    else if (event == Event::n || event == Event::N)
    {
        memory_prompt_model_->stopReading();
        setStatus("Reading stopped: only the rows read so far are shown");
    }
    else
        return true; // the question stays
    memory_prompt_model_.reset();
    return true;
}

Element TuiApp::renderMemoryPrompt()
{
    const auto checkpoint = memory_prompt_model_->memoryCheckpoint();
    const std::string size = checkpoint ? memorySizeText(checkpoint->bytes) : std::string("?");
    const std::string rows = checkpoint ? std::to_string(checkpoint->rows) : std::string("?");
    return hbox({
               text(" The " + rows + " rows read so far take about " + size +
                    " of memory. Continue reading? (y/n) ") |
                   bold,
               filler(),
               text("n: show only the rows read ") | dim,
           }) |
           inverted;
}

// A part not given keeps the cursor's row or column.
void TuiApp::goTo(const std::string &text)
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    const GoToResult result = parseGoTo(text, *view->model());
    if (!result.target)
    {
        setStatus(result.error, true);
        return;
    }
    view->jumpTo(result.target->row.value_or(view->cursorRow()), result.target->column.value_or(view->cursorColumn()));
    setStatus("");
}

// Pasted text (bracketed paste, enabled by runTui()) goes into a prompt as
// one line; a newline would run the prompt and make the next lines commands.
// Elsewhere it is dropped.
bool TuiApp::handlePasteEvent(const Event &event, const Component &target)
{
    static const Event paste_begin = Event::Special("\x1b[200~");
    static const Event paste_end = Event::Special("\x1b[201~");
    if (event == paste_begin || event == paste_end)
    {
        pasting_ = event == paste_begin;
        if (pasting_)
        {
            paste_buffer_.clear();
            discard_paste_ = saving_;
        }
        else
        {
            if (!saving_ && !discard_paste_ && !paste_buffer_.empty())
                pasteCells(paste_buffer_);
            paste_buffer_.clear();
            discard_paste_ = false;
        }
        return true;
    }
    if (saving_)
    {
        paste_buffer_.clear();
        if (pasting_)
            discard_paste_ = true;
        return true;
    }
    if (pasting_ && discard_paste_)
        return true;
    if (!pasting_ || event.is_mouse() || event == Event::Custom)
        return false;
    if (!typingText())
    {
        // In edit mode, into the selected cells.
        if (editing() && !show_filter_dialog_ && !show_text_dialog_ && !show_group_dialog_ && !show_format_dialog_ &&
            !histogram_model_ && !density_model_ &&
            !memory_prompt_model_ &&
            !unsaved_action_)
        {
            if (event == Event::Return || event == Event::Tab)
                paste_buffer_ += event == Event::Return ? '\n' : '\t';
            else if (event.is_character())
                paste_buffer_ += event.character();
        }
        return true;
    }
    // SQL keeps its line breaks, which end "--" comments, and cells theirs.
    if ((sql_prompt_ || edit_prompt_) && !show_filter_dialog_ && !show_text_dialog_ &&
        (event == Event::Return || event == Event::Tab))
    {
        sql_history_cursor_.reset();
        (edit_prompt_ ? edit_input_ : sql_input_) += event == Event::Return ? '\n' : '\t';
        return true;
    }
    if (event == Event::Return || event == Event::Tab)
        return target->OnEvent(Event::Character(" "));
    return !event.is_character(); // other keys are not passed on
}

// Whether character keys go into text being typed.
bool TuiApp::typingText() const
{
    if (show_text_dialog_ || show_group_dialog_ || show_format_dialog_)
        return false;
    if (show_filter_dialog_)
    {
        const auto &conditions = filter_draft_.conditions;
        return filter_line_ >= 1 && filter_line_ <= conditions.size() &&
               filterOpTakesValue(conditions[filter_line_ - 1].op);
    }
    return search_prompt_ || goto_prompt_ || export_prompt_ || sql_prompt_ || edit_prompt_;
}

bool TuiApp::handleEvent(Event event)
{
    if (event == Event::Custom)
        return false;
    if (unsaved_action_ && !event.is_mouse())
        return handleUnsavedPromptEvent(event);
    if (saving_ && !event.is_mouse())
    {
        setStatus("Saving...");
        return true;
    }
    if (edit_prompt_ && !event.is_mouse())
        return handleEditPromptEvent(event);
    // The question can appear while a prompt is open: the keys stay with the
    // prompt (an "n" typed into a search must not stop the reading), and the
    // question is asked once the prompt is closed.
    const bool text_prompt_open = search_prompt_ || goto_prompt_ || export_prompt_ || sql_prompt_;
    if (memory_prompt_model_ && !text_prompt_open && !event.is_mouse())
        return handleMemoryPromptEvent(event);
    if (histogram_model_ && !search_prompt_ && !goto_prompt_ && !sql_prompt_ && !export_prompt_)
        return handleHistogramEvent(event);
    if (density_model_ && !search_prompt_ && !goto_prompt_ && !sql_prompt_ && !export_prompt_)
        return handleDensityEvent(event);
    if (search_prompt_ && !event.is_mouse())
        return handleSearchPromptEvent(event);
    if (goto_prompt_ && !event.is_mouse())
        return handleGoToPromptEvent(event);
    if (export_prompt_ && !event.is_mouse())
        return handleExportPromptEvent(event);
    if (sql_prompt_ && !event.is_mouse())
        return handleSqlPromptEvent(event);

    if (event.is_mouse())
    {
        Mouse &mouse = event.mouse();
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed)
        {
            for (size_t i = 0; i < tab_boxes_.size(); ++i)
            {
                if (tab_boxes_[i].Contain(mouse.x, mouse.y))
                {
                    showSheet(i);
                    return true;
                }
            }
        }
        return false;
    }

    // A message stays until the next key, and a key cancels a waiting n / N.
    status_message_.clear();
    pending_jump_ = SearchJump::None;

    if (confirm_overwrite_ != Overwrite::None)
    {
        const Overwrite action = std::exchange(confirm_overwrite_, Overwrite::None);
        const bool yes = event == Event::y || event == Event::Y;
        if (action == Overwrite::Convert)
            yes ? startConversion() : setStatus("Not converted");
        else if (yes)
            startExport();
        else
        {
            export_model_.reset();
            setStatus("Not exported");
        }
        return true;
    }
    if (converting_ && event == Event::Escape)
    {
        convert_thread_.request_stop();
        return true;
    }
    if (event == Event::Character(':'))
    {
        if (!book_ || !book_->supportsSql())
            setStatus("SQL is available for DuckDB, SQLite, Parquet and JSON files", true);
        else if (sql_running_)
            setStatus("SQL is still running", true);
        else
        {
            sql_prompt_ = true;
            sql_input_.clear();
            sql_completion_.reset();
            loadSqlSchema();
        }
        return true;
    }
    if (event == Event::w)
    {
        if (converting_)
            setStatus("Another export or conversion is running (Esc: cancel it)", true);
        else
            openExportPrompt();
        return true;
    }
    if (event == Event::P)
    {
        convertTo(ExportFormat::Parquet);
        return true;
    }
    if (event == Event::D)
    {
        convertTo(ExportFormat::DuckDb);
        return true;
    }

    const size_t sheet_count = views_.size();
    if (event == Event::q || event == Event::Q)
    {
        checkUnsaved([this] { app_.Exit(); });
        return true;
    }
    if (event == Event::I)
    {
        if (!book_ || !book_->supportsEditing())
            setStatus("Only CSV, TSV and Excel files can be edited", true);
        else if (editing())
            checkUnsaved([this] { setEditing(false); });
        else
            setEditing(true);
        return true;
    }
    if (event == Event::i || event == Event::F2)
    {
        if (requireEditMode())
            editCell();
        return true;
    }
    if (event == Event::u || event == Event::CtrlR)
    {
        if (requireEditMode())
            undoEdit(event == Event::CtrlR);
        return true;
    }
    if (event == Event::CtrlS)
    {
        if (requireEditMode())
            save();
        return true;
    }
    if (event == Event::Delete || event == Event::Backspace)
    {
        if (requireEditMode())
            clearSelectedCells();
        return true;
    }
    if (event == Event::Character('?') || event == Event::F1)
    {
        showHelp();
        return true;
    }
    if (sheet_count > 1 && (event == Event::Tab || event == Event::Character(']')))
    {
        showSheet((sheet_ + 1) % sheet_count);
        return true;
    }
    if (sheet_count > 1 && (event == Event::TabReverse || event == Event::Character('[')))
    {
        showSheet((sheet_ + sheet_count - 1) % sheet_count);
        return true;
    }
    if (event == Event::s)
    {
        show_sidebar_ = !show_sidebar_;
        return true;
    }
    if (event == Event::S)
    {
        showColumnStats();
        return true;
    }
    if (event == Event::B)
    {
        if (TuiTableView *view = currentView(); view && view->model() && view->model()->columnCount() > 0)
            showHistogram(view->cursorColumn());
        return true;
    }
    if (event == Event::M)
    {
        TuiTableView *view = currentView();
        if (!view || !view->model() || view->model()->columnCount() == 0)
            return true;
        const TuiTableView::Selection selection = view->selection();
        size_t x_column = selection.col_left;
        size_t y_column = selection.col_right;
        if (x_column == y_column)
        {
            // Columns holding numbers or dates near the cursor's, judged by
            // the rows around it, which are loaded.
            const size_t row = view->cursorRow();
            const ScatterColumns columns = defaultScatterColumns(*view->model(), view->cursorColumn(),
                                                                 view->hiddenColumns(), row > 50 ? row - 50 : 0, row + 50);
            x_column = columns.x;
            y_column = columns.y;
        }
        showDensity(x_column, y_column);
        return true;
    }
    if (event == Event::A)
    {
        if (converting_)
            setStatus("Another export or conversion is running (Esc: cancel it)", true);
        else
            openGroupDialog();
        return true;
    }
    if (event == Event::Character('%'))
    {
        openFormatDialog();
        return true;
    }
    if (event == Event::Character('|'))
    {
        TuiTableView *view = currentView();
        if (!view || !view->model() || view->model()->columnCount() == 0)
            return true;
        const size_t column = view->cursorColumn();
        if (view->frozenColumns() == column + 1)
        {
            view->freezeColumns(0);
            setStatus("Columns unfrozen");
        }
        else if (column + 1 >= view->model()->columnCount())
            setStatus("The last column cannot be frozen", true);
        else
        {
            view->freezeColumns(column + 1);
            setStatus("Columns frozen up to " + singleLine(view->model()->columnName(column)));
        }
        return true;
    }
    if (event == Event::x || event == Event::X)
    {
        TuiTableView *view = currentView();
        if (!view || !view->model() || view->model()->columnCount() == 0)
            return true;
        if (event == Event::X)
        {
            view->showAllColumns();
            setStatus("All columns are shown");
            return true;
        }
        const TuiTableView::Selection sel = view->selection();
        if (view->hiddenColumns().hidesAll(sel.col_left, sel.col_right, view->model()->columnCount()))
            setStatus("The last column cannot be hidden", true);
        else
            view->hideColumns(sel.col_left, sel.col_right);
        return true;
    }
    // Reading the file again loses the edits.
    if (event == Event::H)
    {
        if (!refuseReopen())
            checkUnsaved([this] { toggleFirstRow(); });
        return true;
    }
    if (event == Event::Character('#'))
    {
        // Checked before asking about unsaved edits, which the answer may discard.
        if (!skipMetaLinesAvailable())
            setStatus("Only text files can skip lines", true);
        else if (!refuseReopen())
            checkUnsaved([this] { setSkipMetaLines(!skipsMetaLines()); });
        return true;
    }
    if (event == Event::E)
    {
        if (!skipMetaLinesAvailable())
        {
            setStatus("Only text files have an encoding", true);
            return true;
        }
        if (refuseReopen())
            return true;
        const auto it = std::find(TEXT_ENCODINGS.begin(), TEXT_ENCODINGS.end(), textEncoding());
        const size_t next = it == TEXT_ENCODINGS.end() ? 0 : (it - TEXT_ENCODINGS.begin() + 1) % TEXT_ENCODINGS.size();
        checkUnsaved([this, encoding = TEXT_ENCODINGS[next]] { setTextEncoding(encoding); });
        return true;
    }
    if (event == Event::T)
    {
        setFollow(!follow_);
        return true;
    }
    if (event == Event::R)
    {
        auto_revert_ = !auto_revert_;
        setStatus(auto_revert_ ? "Auto Revert on" : "Auto Revert off");
        if (auto_revert_)
            revertIfChanged();
        return true;
    }
    if (path_.empty())
        return false;

    if (event == Event::r)
    {
        if (!refuseReopen())
            checkUnsaved([this] { revert(); }, true);
        return true;
    }
    if (event == Event::e)
    {
        openWithFileBrowser(utf8Path(path_));
        return true;
    }
    if (event == Event::y || event == Event::c)
    {
        copySelection();
        return true;
    }
    if (event == Event::Character('/'))
    {
        search_prompt_ = true;
        search_input_.clear();
        return true;
    }
    if (event == Event::CtrlG)
    {
        if (currentView() && currentView()->model())
        {
            goto_prompt_ = true;
            goto_input_.clear();
        }
        return true;
    }
    if (event == Event::n || event == Event::N)
    {
        jumpToHit(event == Event::n);
        return true;
    }
    if (event == Event::f)
    {
        openFilterDialog();
        return true;
    }
    if (event == Event::Character('=') || event == Event::Character('!'))
    {
        quickFilter(event == Event::Character('='));
        return true;
    }
    if (event == Event::F)
    {
        clearFilter();
        return true;
    }
    if (event == Event::o)
    {
        TuiTableView *view = currentView();
        if (view && !view->cycleSort())
            setStatus("Still opening (cannot sort yet)", true);
        return true;
    }
    if (event == Event::Character('<') || event == Event::Character('>'))
    {
        if (TuiTableView *view = currentView())
            view->widenColumn(event == Event::Character('<') ? -COLUMN_WIDTH_STEP : COLUMN_WIDTH_STEP);
        return true;
    }
    return false;
}

bool TuiApp::handleTextDialogEvent(Event event)
{
    if (event == Event::Custom)
        return false;
    // renderTextDialog() keeps the scroll position within range.
    const size_t last = dialogRows().size();
    const size_t page = static_cast<size_t>(std::max(dialog_page_ - 1, 1));
    if (event.is_mouse())
    {
        Mouse &mouse = event.mouse();
        if (mouse.button == Mouse::WheelUp)
            dialog_scroll_ = dialog_scroll_ > 3 ? dialog_scroll_ - 3 : 0;
        else if (mouse.button == Mouse::WheelDown)
            dialog_scroll_ = std::min(dialog_scroll_ + 3, last);
        return true;
    }
    if (event == Event::Escape || event == Event::Return || event == Event::q)
        show_text_dialog_ = false;
    else if (event == Event::ArrowUp || event == Event::k)
        dialog_scroll_ = dialog_scroll_ > 0 ? dialog_scroll_ - 1 : 0;
    else if (event == Event::ArrowDown || event == Event::j)
        dialog_scroll_ = std::min(dialog_scroll_ + 1, last);
    else if (event == Event::PageUp || event == Event::CtrlB)
        dialog_scroll_ = dialog_scroll_ > page ? dialog_scroll_ - page : 0;
    else if (event == Event::PageDown || event == Event::CtrlF || event == Event::Character(' '))
        dialog_scroll_ = std::min(dialog_scroll_ + page, last);
    else if (event == Event::Home || event == Event::g)
        dialog_scroll_ = 0;
    else if (event == Event::End || event == Event::G)
        dialog_scroll_ = last;
    else if (event == Event::y || event == Event::c)
    {
        std::string text;
        for (size_t i = 0; i < dialog_lines_.size(); ++i)
            text += (i > 0 ? "\n" : "") + dialog_lines_[i];
        copyToClipboard(text);
        setStatus("Copied");
    }
    return true;
}

Element TuiApp::renderMain()
{
    Elements parts;
    TuiTableView *view = currentView();
    if (views_.size() > 1)
        parts.push_back(renderTabs());
    else
        tab_boxes_.clear();

    Element body;
    if (histogram_model_)
        body = renderHistogram();
    else if (density_model_)
        body = renderDensity();
    else if (view)
    {
        view->setSearchState(&search_);
        body = view->Render();
    }
    else
        body = vbox({
                   text("TableSkim " TABLESKIM_VERSION_FULL) | bold | center,
                   text(""),
                   text("?: help    q: quit") | dim | center,
               }) |
               center;
    if (show_sidebar_ && !histogram_model_ && !density_model_)
    {
        const int width = std::max(Terminal::Size().dimx / 3, 20);
        body = hbox({body | flex, separator(), renderSidebar(width) | size(WIDTH, EQUAL, width)});
    }
    parts.push_back(body | flex);
    parts.push_back(unsaved_action_ ? renderUnsavedPrompt()
                    : edit_prompt_ ? renderEditPrompt()
                    : search_prompt_ ? renderSearchPrompt()
                    : sql_prompt_  ? renderSqlPrompt()
                    : goto_prompt_ ? renderGoToPrompt()
                    : export_prompt_ ? renderExportPrompt()
                    : memory_prompt_model_ ? renderMemoryPrompt()
                                   : renderStatusBar());
    return vbox(std::move(parts));
}

Element TuiApp::renderTabs()
{
    const auto &names = book_->sheetNames();
    tab_boxes_.resize(names.size());
    Elements tabs;
    for (size_t i = 0; i < names.size(); ++i)
    {
        Element tab = text(" " + names[i] + " ");
        tab = i == sheet_ ? tab | bold | inverted : tab | dim;
        tabs.push_back(tab | reflect(tab_boxes_[i]));
        tabs.push_back(text("│") | color(Color::GrayDark));
    }
    return hbox(std::move(tabs)) | xframe;
}

// Starts the statistics of the cursor's column, unless they are already
// shown, in which case the sidebar goes back to the cell.
void TuiApp::showColumnStats()
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return;
    const size_t column = view->cursorColumn();
    const ColumnStatsState state = view->model()->columnStatsState();
    const bool same = state.active && state.column == column;
    if (same && show_sidebar_ && sidebar_mode_ == SidebarMode::Stats)
    {
        sidebar_mode_ = SidebarMode::Cell;
        return;
    }
    // Statistics already computed (or being computed) are kept.
    if (!same)
        view->model()->startColumnStats(column);
    show_sidebar_ = true;
    sidebar_mode_ = SidebarMode::Stats;
}

void TuiApp::showHistogram(size_t column)
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    if (histogram_model_ && histogram_model_ != view->model())
        histogram_model_->cancelHistogram();
    histogram_model_ = view->model();
    histogram_model_->startHistogram(column);
    histogram_selected_ = 0;
    histogram_top_ = 0;
}

void TuiApp::closeHistogram()
{
    if (histogram_model_)
        histogram_model_->cancelHistogram();
    histogram_model_.reset();
}

bool TuiApp::handleHistogramEvent(Event event)
{
    if (event.is_mouse() || event == Event::Custom)
        return false;
    const HistogramState state = histogram_model_->histogramState();
    const Histogram &hg = state.histogram;
    const size_t bars = hg.binCount() > 0 ? hg.binCount() : hg.top.size();
    const size_t columns = histogram_model_->columnCount();
    if (event == Event::Escape || event == Event::B || event == Event::q)
        closeHistogram();
    else if ((event == Event::ArrowUp || event == Event::k) && histogram_selected_ > 0)
        --histogram_selected_;
    else if ((event == Event::ArrowDown || event == Event::j) && histogram_selected_ + 1 < bars)
        ++histogram_selected_;
    else if (event == Event::ArrowLeft || event == Event::h || event == Event::ArrowRight || event == Event::l)
    {
        const bool forward = event == Event::ArrowRight || event == Event::l;
        const HiddenColumns &hidden = hiddenColumnsOf(histogram_model_);
        if (const auto column = nextVisibleColumn(hidden, state.column, forward, columns, false))
            showHistogram(*column);
    }
    else if (event == Event::F)
        clearFilter();
    else if (event == Event::Return && state.done && histogram_selected_ < bars)
    {
        if (!histogram_model_->viewCapabilities().filter)
            return true;
        ViewSpec spec = histogram_model_->requestedView();
        if (hg.binCount() > 0)
        {
            for (FilterCondition &condition : histogramBinConditions(hg, state.column, histogram_selected_))
                addFilterCondition(spec, std::move(condition));
        }
        else
            addFilterCondition(spec, cellFilterCondition(state.column, hg.top[histogram_selected_].value, true));
        // The histogram starts over on the rows left.
        histogram_model_->setView(std::move(spec));
        histogram_selected_ = 0;
        histogram_top_ = 0;
    }
    return true;
}

Element TuiApp::renderHistogram()
{
    const auto &model = histogram_model_;
    const HistogramState state = model->histogramState();
    const Histogram &hg = state.histogram;
    const int width = Terminal::Size().dimx;
    // The title, a separator, the hint and the status bar.
    const int height = std::max(Terminal::Size().dimy - 4 - (views_.size() > 1 ? 1 : 0), 3);

    std::string status;
    if (!state.done)
        status = (state.counting ? "counting… " : "reading… ") + std::to_string(state.scanned_rows) + " rows";
    else
        status = histogramSummary(hg) + (model->view().filtered() ? " (filtered rows)" : "");
    const std::string title = " Histogram: " + singleLine(model->columnName(state.column)) + "  ";

    // Rows: a label, a bar, and the count with its share.
    std::vector<std::string> labels;
    std::vector<size_t> counts;
    size_t total = hg.values;
    if (hg.binCount() > 0)
    {
        for (size_t i = 0; i < hg.binCount(); ++i)
        {
            labels.push_back(histogramBinLabel(hg, i));
            counts.push_back(hg.counts[i]);
        }
    }
    else if (state.done)
    {
        for (const ValueCount &value : hg.top)
        {
            labels.push_back(cellDisplayText(value.value));
            counts.push_back(value.count);
        }
        total = hg.rows;
    }
    Elements rows;
    if (!labels.empty())
    {
        histogram_selected_ = std::min(histogram_selected_, labels.size() - 1);
        if (histogram_selected_ < histogram_top_)
            histogram_top_ = histogram_selected_;
        if (histogram_selected_ >= histogram_top_ + static_cast<size_t>(height))
            histogram_top_ = histogram_selected_ + 1 - static_cast<size_t>(height);
        int label_width = 0;
        int count_width = 0;
        size_t most = 1;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            label_width = std::max(label_width, string_width(labels[i]));
            count_width = std::max(count_width, string_width(std::to_string(counts[i]) + " " + percentText(counts[i], total)));
            most = std::max(most, counts[i]);
        }
        label_width = std::min(label_width, width / 3);
        const int bar_width = std::max(width - label_width - count_width - 6, 4);
        static const char *const partial[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
        for (size_t i = histogram_top_; i < labels.size() && i < histogram_top_ + static_cast<size_t>(height); ++i)
        {
            const auto eighths = static_cast<size_t>(std::llround(static_cast<double>(counts[i]) /
                                                                  static_cast<double>(most) * bar_width * 8));
            std::string bar;
            for (size_t k = 0; k < eighths / 8; ++k)
                bar += "█";
            bar += partial[eighths % 8];
            const std::string count = std::to_string(counts[i]) + " " + percentText(counts[i], total);
            Element line = hbox({
                text(" " + fitText(labels[i], label_width, TextAlign::Right) + " │"),
                text(bar) | color(Color::CyanLight),
                text(" " + count),
                filler(),
            });
            if (i == histogram_selected_)
                line = line | inverted;
            rows.push_back(line);
        }
    }
    else
        rows.push_back(text(state.done ? " No values" : " Reading the column…") | dim);

    std::string hint = " ↑↓: bar  ←→: column  Enter: filter to the bar  F: clear the filter  Esc: back";
    if (!labels.empty() && hg.binCount() > 0)
        hint = " " + histogramBinRange(hg, histogram_selected_) + "   " + hint.substr(1);
    return vbox({
        hbox({text(title) | bold, text(fitText(status, std::max(width - string_width(title), 1), TextAlign::Left)) | dim}),
        separator(),
        vbox(std::move(rows)) | flex,
        text(fitText(hint, width, TextAlign::Left)) | dim,
    });
}

void TuiApp::showDensity(size_t x_column, size_t y_column)
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    if (density_model_ && density_model_ != view->model())
        density_model_->cancelScatter();
    density_model_ = view->model();
    // About two characters per X bin, so that cells look square, and a line
    // per Y bin (less the title, separator, X labels, hint and status bar).
    const auto size = Terminal::Size();
    ScatterOptions options;
    options.density_bins_x = static_cast<size_t>(std::max((size.dimx - 12) / 2, 4));
    options.density_bins_y = static_cast<size_t>(std::max(size.dimy - 6 - (views_.size() > 1 ? 1 : 0), 4));
    options.max_points = 0;
    density_model_->startScatter(x_column, y_column, options);
    density_chosen_ = false;
}

void TuiApp::closeDensity()
{
    if (density_model_)
        density_model_->cancelScatter();
    density_model_.reset();
}

bool TuiApp::handleDensityEvent(Event event)
{
    if (event.is_mouse() || event == Event::Custom)
        return false;
    const ScatterState state = density_model_->scatterState();
    const ScatterPlot &plot = state.plot;
    const size_t x_bins = plot.xBinCount();
    const size_t y_bins = plot.y_bins.binCount();
    const size_t columns = density_model_->columnCount();
    auto choose = [this] { density_chosen_ = true; };
    // x / y and X / Y step through the columns, skipping hidden ones.
    auto otherColumn = [&](size_t column, bool forward)
    { return nextVisibleColumn(hiddenColumnsOf(density_model_), column, forward, columns, true).value_or(column); };
    const bool boxes = density_box_ && plot.categorical();
    if (event == Event::Escape || event == Event::M || event == Event::q)
        closeDensity();
    else if (event == Event::b && plot.categorical())
        density_box_ = !density_box_;
    else if (boxes && (event == Event::ArrowUp || event == Event::k))
        box_selected_ = box_selected_ > 0 ? box_selected_ - 1 : 0;
    else if (boxes && (event == Event::ArrowDown || event == Event::j))
        box_selected_ = std::min(box_selected_ + 1, x_bins > 0 ? x_bins - 1 : 0);
    else if (boxes && event == Event::Return && state.done)
    {
        if (!density_model_->viewCapabilities().filter)
            return true;
        ViewSpec spec = density_model_->requestedView();
        for (FilterCondition &condition : scatterCategoryConditions(plot, state.x_column, state.y_column, box_selected_))
            addFilterCondition(spec, std::move(condition));
        density_model_->setView(std::move(spec));
    }
    else if (boxes)
    {
        // The keys below are the density's.
        if (columns > 0 && (event == Event::x || event == Event::X))
            showDensity(otherColumn(state.x_column, event == Event::x), state.y_column);
        else if (columns > 0 && (event == Event::y || event == Event::Y))
            showDensity(state.x_column, otherColumn(state.y_column, event == Event::y));
        else if (event == Event::t)
            showDensity(state.y_column, state.x_column);
        else if (event == Event::F)
            clearFilter();
    }
    else if (event == Event::ArrowUp || event == Event::k)
    {
        if (density_y_ + 1 < y_bins)
            ++density_y_;
        choose();
    }
    else if (event == Event::ArrowDown || event == Event::j)
    {
        if (density_y_ > 0)
            --density_y_;
        choose();
    }
    else if (event == Event::ArrowLeft || event == Event::h)
    {
        if (density_x_ > 0)
            --density_x_;
        choose();
    }
    else if (event == Event::ArrowRight || event == Event::l)
    {
        if (density_x_ + 1 < x_bins)
            ++density_x_;
        choose();
    }
    else if (columns > 0 && (event == Event::x || event == Event::X))
        showDensity(otherColumn(state.x_column, event == Event::x), state.y_column);
    else if (columns > 0 && (event == Event::y || event == Event::Y))
        showDensity(state.x_column, otherColumn(state.y_column, event == Event::y));
    else if (event == Event::t)
        showDensity(state.y_column, state.x_column);
    else if (event == Event::L)
        density_log_ = !density_log_;
    else if (event == Event::F)
        clearFilter();
    else if (event == Event::Return && state.done && density_x_ < x_bins && density_y_ < y_bins)
    {
        if (!density_model_->viewCapabilities().filter)
            return true;
        ViewSpec spec = density_model_->requestedView();
        for (FilterCondition &condition :
             densityCellConditions(plot, state.x_column, state.y_column, density_x_, density_y_))
            addFilterCondition(spec, std::move(condition));
        // The density starts over on the rows left.
        density_model_->setView(std::move(spec));
    }
    return true;
}

Element TuiApp::renderDensity()
{
    const auto &model = density_model_;
    const ScatterState state = model->scatterState();
    const ScatterPlot &plot = state.plot;
    const int width = Terminal::Size().dimx;
    // The title, a separator, the X labels, the hint and the status bar.
    const int height = std::max(Terminal::Size().dimy - 5 - (views_.size() > 1 ? 1 : 0), 2);
    if (state.generation != density_generation_)
    {
        density_generation_ = state.generation;
        density_chosen_ = false;
    }
    // Text on X is best seen as box plots; once the layout is known.
    if ((state.counting || state.done) && plot.categorical() != density_categorical_)
    {
        density_categorical_ = plot.categorical();
        density_box_ = density_categorical_;
        box_selected_ = 0;
    }

    std::string status;
    if (!state.done)
        status = (state.counting ? "counting… " : "reading… ") + std::to_string(state.scanned_rows) + " rows";
    else
    {
        status = scatterSummary(plot, false) + (model->view().filtered() ? " (filtered rows)" : "");
        if (const std::string stats = scatterStatsText(plot); !stats.empty())
            status += "  " + stats;
    }
    const bool boxes = density_box_ && plot.categorical();
    const std::string title = std::string(boxes ? " Box plot: " : " Density: ") +
                              singleLine(model->columnName(state.x_column)) + " × " +
                              singleLine(model->columnName(state.y_column)) + "  ";
    std::string hint = plot.categorical()
                           ? " ↑↓←→: cell  b: box plot  x/X y/Y: columns  t: swap  L: log scale  Enter: filter  F: clear filter  Esc: back"
                           : " ↑↓←→: cell  x/X y/Y: columns  t: swap  L: log scale  Enter: filter  F: clear filter  Esc: back";

    Elements lines;
    const size_t x_bins = plot.xBinCount();
    const size_t y_bins = plot.y_bins.binCount();
    if (plot.plottable() && boxes)
        lines = renderBoxPlot(plot, width, height, hint);
    else if (plot.plottable())
    {
        size_t most = 0;
        size_t densest = 0;
        for (size_t i = 0; i < plot.counts.size(); ++i)
        {
            if (plot.counts[i] > most)
            {
                most = plot.counts[i];
                densest = i;
            }
        }
        if (!density_chosen_ && most > 0)
        {
            density_x_ = densest / y_bins;
            density_y_ = densest % y_bins;
            density_chosen_ = state.done;
        }
        density_x_ = std::min(density_x_, x_bins - 1);
        density_y_ = std::min(density_y_, y_bins - 1);

        // Y labels on the rows of the axis ticks.
        std::vector<std::string> y_labels(y_bins);
        int label_width = 0;
        for (const double edge : plot.y_axis.edges)
        {
            if (const auto bin = histogramBin(plot.y_bins.edges, edge))
            {
                y_labels[*bin] = histogramEdgeText(plot.y_axis, edge);
                label_width = std::max(label_width, string_width(y_labels[*bin]));
            }
        }
        const int grid_width = std::max(width - label_width - 3, 1);
        const int cell_width = std::max(1, grid_width / static_cast<int>(x_bins));
        // The rows around the selected one, if not all fit; the highest first.
        const size_t shown = std::min(y_bins, static_cast<size_t>(height));
        const size_t first = density_y_ >= shown ? density_y_ + 1 - shown : 0;
        for (size_t r = 0; r < shown; ++r)
        {
            const size_t y_bin = first + shown - 1 - r;
            Elements cells;
            cells.push_back(text(" " + fitText(y_labels[y_bin], label_width, TextAlign::Right) + " │"));
            for (size_t x_bin = 0; x_bin < x_bins; ++x_bin)
            {
                const size_t count = plot.count(x_bin, y_bin);
                const bool selected = x_bin == density_x_ && y_bin == density_y_;
                std::string content(static_cast<size_t>(cell_width), ' ');
                if (selected)
                    content = cell_width >= 2 ? "[" + std::string(static_cast<size_t>(cell_width - 2), ' ') + "]" : "▪";
                Element cell = text(content);
                if (count > 0)
                {
                    const double t = densityScale(count, most, density_log_);
                    const auto rgb = densityColor(t);
                    cell = cell | bgcolor(Color::RGB(rgb[0], rgb[1], rgb[2])) | color(t > 0.6 ? Color::Black : Color::White);
                }
                cells.push_back(cell | (selected ? bold : nothing));
            }
            lines.push_back(hbox(std::move(cells)));
        }
        // X labels at the ticks, as many as fit; a category's under its
        // column, cut to it.
        const auto offset = static_cast<size_t>(label_width + 3);
        std::string axis(offset + x_bins * static_cast<size_t>(cell_width), ' ');
        if (plot.categorical())
        {
            axis = std::string(offset, ' ');
            for (size_t i = 0; i < x_bins; ++i)
            {
                const std::string label = singleLine(scatterCategoryText(plot, i));
                axis += fitText(label, cell_width - 1, TextAlign::Left) + (cell_width > 1 ? " " : "");
            }
        }
        size_t free_from = 0;
        for (const double edge : plot.categorical() ? std::vector<double>{} : plot.x_axis.edges)
        {
            const auto bin = histogramBin(plot.x_bins.edges, edge);
            if (!bin)
                continue;
            const size_t at = offset + *bin * static_cast<size_t>(cell_width);
            const std::string label = histogramEdgeText(plot.x_axis, edge);
            if (at < free_from || at + label.size() > axis.size())
                continue;
            axis.replace(at, label.size(), label);
            free_from = at + label.size() + 2;
        }
        lines.push_back(text(axis) | dim);
        hint = " " + densityCellText(plot, density_x_, density_y_) + "  " + hint;
    }
    else
        lines.push_back(text(state.done ? " " + scatterSummary(plot, false) : " Reading the columns…") | dim);

    return vbox({
        hbox({text(title) | bold, text(fitText(status, std::max(width - string_width(title), 1), TextAlign::Left)) | dim}),
        separator(),
        vbox(std::move(lines)) | flex,
        text(fitText(hint, width, TextAlign::Left)) | dim,
    });
}

// One line per category: its whiskers ├──┤, box ███ from Q1 to Q3 with the
// median ┃, and outliers ∘, on a common Y scale; the selected category's
// numbers go in the hint.
Elements TuiApp::renderBoxPlot(const ScatterPlot &plot, int width, int height, std::string &hint)
{
    const size_t categories = plot.boxes.size();
    box_selected_ = std::min(box_selected_, categories > 0 ? categories - 1 : 0);
    int label_width = 0;
    for (size_t i = 0; i < categories; ++i)
        label_width = std::max(label_width, string_width(singleLine(scatterCategoryText(plot, i))));
    label_width = std::clamp(label_width, 4, std::max(width / 4, 4));
    const int scale_width = std::max(width - label_width - 4, 10);
    const double low = plot.y_axis.edges.empty() ? 0 : plot.y_axis.edges.front();
    double high = plot.y_axis.edges.empty() ? 1 : plot.y_axis.edges.back();
    if (!(high > low))
        high = low + 1;
    const auto column = [&](double value)
    {
        const double at = (value - low) / (high - low) * (scale_width - 1);
        return std::clamp(static_cast<int>(std::lround(at)), 0, scale_width - 1);
    };

    Elements lines;
    // The rows around the selected one, if not all fit.
    const size_t shown = std::min(categories, static_cast<size_t>(std::max(height - 1, 1)));
    const size_t first = box_selected_ >= shown ? box_selected_ + 1 - shown : 0;
    for (size_t i = first; i < first + shown; ++i)
    {
        const BoxStats &box = plot.boxes[i];
        std::vector<std::string> chars(static_cast<size_t>(scale_width), " ");
        std::vector<int> kinds(static_cast<size_t>(scale_width), 0); // 0 none, 1 whisker, 2 box, 3 median, 4 outlier
        if (box.count > 0)
        {
            const int w0 = column(box.low_whisker);
            const int w1 = column(box.high_whisker);
            const int b0 = column(box.q1);
            const int b1 = column(box.q3);
            for (int c = w0; c <= w1; ++c)
            {
                chars[static_cast<size_t>(c)] = "─";
                kinds[static_cast<size_t>(c)] = 1;
            }
            chars[static_cast<size_t>(w0)] = "├";
            chars[static_cast<size_t>(w1)] = "┤";
            for (int c = b0; c <= b1; ++c)
            {
                chars[static_cast<size_t>(c)] = "█";
                kinds[static_cast<size_t>(c)] = 2;
            }
            chars[static_cast<size_t>(column(box.median))] = "┃";
            kinds[static_cast<size_t>(column(box.median))] = 3;
            for (const double value : box.outliers)
            {
                chars[static_cast<size_t>(column(value))] = "∘";
                kinds[static_cast<size_t>(column(value))] = 4;
            }
        }
        Elements cells;
        const std::string label = fitText(singleLine(scatterCategoryText(plot, i)), label_width, TextAlign::Right);
        Element label_element = text(" " + label + " ");
        cells.push_back(i == box_selected_ ? label_element | inverted : label_element);
        cells.push_back(text("│"));
        for (size_t c = 0; c < chars.size(); ++c)
        {
            Element cell = text(chars[c]);
            if (kinds[c] == 2)
                cell = cell | color(Color::Cyan);
            else if (kinds[c] == 3)
                cell = cell | bgcolor(Color::Cyan) | color(Color::Black) | bold;
            else if (kinds[c] == 4)
                cell = cell | color(Color::Red);
            cells.push_back(std::move(cell));
        }
        lines.push_back(hbox(std::move(cells)));
    }
    // The scale's labels at the ticks, as many as fit.
    const auto offset = static_cast<size_t>(label_width + 3);
    std::string axis(offset + static_cast<size_t>(scale_width), ' ');
    size_t free_from = 0;
    for (const double edge : plot.y_axis.edges)
    {
        const size_t at = offset + static_cast<size_t>(column(edge));
        const std::string label = histogramEdgeText(plot.y_axis, edge);
        if (at < free_from || at + label.size() > axis.size())
            continue;
        axis.replace(at, label.size(), label);
        free_from = at + label.size() + 2;
    }
    lines.push_back(text(axis) | dim);
    hint = " " + boxPlotText(plot, box_selected_) +
           "   ↑↓: category  b: density  x/X y/Y: columns  t: swap  Enter: filter  F: clear filter  Esc: back";
    return lines;
}

Element TuiApp::renderStatsSidebar(int width)
{
    TuiTableView *view = currentView();
    const auto &model = view->model();
    const ColumnStatsState state = model->columnStatsState();
    if (!state.active)
        return vbox({text(" Statistics") | bold, separator(), text(" S: the cursor's column") | dim});

    const int inner = std::max(width - 2, 4);
    Elements lines;
    std::string status;
    if (!state.done)
        status = "computing… " + std::to_string(state.scanned_rows) + " rows";
    else
        status = rowCountText({state.scanned_rows, RowCount::Kind::Exact}) + (model->view().filtered() ? " (filtered)" : "");
    lines.push_back(text(" " + fitText(status, inner, TextAlign::Left)) | dim);
    // Nothing is published until the first rows have been scanned.
    if (state.done || state.stats.rows > 0)
    {
        const std::vector<StatsLine> summary = columnStatsLines(state.stats);
        int label_width = 0;
        for (const StatsLine &line : summary)
            label_width = std::max(label_width, string_width(line.label));
        for (const StatsLine &line : summary)
        {
            lines.push_back(hbox({text(" " + fitText(line.label, label_width, TextAlign::Left) + " ") | dim,
                                  text(fitText(line.value, std::max(inner - label_width - 1, 1), TextAlign::Left))}));
        }
        if (!state.stats.top.empty())
        {
            // Count and percentage columns as wide as their widest entry.
            int count_width = string_width("count");
            int percent_width = 1;
            for (const ValueCount &value : state.stats.top)
            {
                count_width = std::max(count_width, string_width(std::to_string(value.count)));
                percent_width = std::max(percent_width, string_width(percentText(value.count, state.stats.rows)));
            }
            const int value_width = std::max(inner - count_width - percent_width - 2, 1);
            auto row = [&](const std::string &value, const std::string &count, const std::string &percent)
            {
                return " " + fitText(value, value_width, TextAlign::Left) + " " +
                       fitText(count, count_width, TextAlign::Right) + " " +
                       fitText(percent, percent_width, TextAlign::Right);
            };
            lines.push_back(separator());
            lines.push_back(text(row("most frequent", "count", "%")) | dim);
            for (const ValueCount &value : state.stats.top)
                lines.push_back(text(row(cellDisplayText(value.value), std::to_string(value.count),
                                         percentText(value.count, state.stats.rows))));
        }
    }
    const std::string title = " Statistics: " + singleLine(model->columnName(state.column));
    return vbox({text(fitText(title, width, TextAlign::Left)) | bold, separator(), vbox(std::move(lines)) | frame | flex});
}

Element TuiApp::renderSidebar(int width)
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return text("");
    if (sidebar_mode_ == SidebarMode::Stats)
        return renderStatsSidebar(width);
    const size_t row = view->cursorRow();
    const size_t col = view->cursorColumn();
    const CellRef cell = view->model()->peek(row, col);
    Element body;
    switch (cell.state)
    {
    case CellState::Ready:
    {
        Elements lines;
        for (const std::string &line : splitLines(formatCell(*cell.value)))
        {
            for (const std::string &row : wrapText(line, width))
                lines.push_back(text(row));
        }
        body = vbox(std::move(lines));
        break;
    }
    case CellState::Loading:
        body = text("…") | dim;
        break;
    default:
        body = text("");
        break;
    }
    return vbox({text(" " + cellName(row, col)) | bold, separator(), body | frame | flex});
}

Element TuiApp::renderStatusBar()
{
    Elements left;
    Elements right;
    if (book_)
    {
        if (editing())
            left.push_back(text(" EDIT ") | bold | bgcolor(Color::Red) | color(Color::White));
        // "*": unsaved changes.
        left.push_back(text(std::string(book_->modified() ? " *" : " ") + utf8String(utf8Path(path_).filename()) + " ") |
                       bold);
        if (changed_on_disk_)
            left.push_back(text("(changed on disk) ") | color(Color::Red));
        TuiTableView *view = currentView();
        if (view && view->model())
        {
            const auto &model = view->model();
            if (views_.size() > 1)
                left.push_back(text("│ " + model->name() + " "));
            if (model->columnCount() > 0)
            {
                const TuiTableView::Selection sel = view->selection();
                std::string where = cellName(view->cursorRow(), view->cursorColumn());
                if (view->hasRangeSelection())
                    where = cellName(sel.row_top, sel.col_left) + ":" + cellName(sel.row_bottom, sel.col_right) +
                            " (" + std::to_string(sel.row_bottom - sel.row_top + 1) + "×" +
                            std::to_string(view->hiddenColumns().visibleIn(sel.col_left, sel.col_right)) + ")";
                if (view->selecting())
                    where += " SELECT";
                left.push_back(text("│ " + where + " "));
            }
            right.push_back(text(filteredRowCountText(model->rowCount(), model->unfilteredRowCount(),
                                                      view->appliedView().filtered()) +
                                 " × " + std::to_string(model->columnCount()) + " cols " +
                                 (model->readingStopped() ? "(partly read) " : "")));
            if (const auto &sort = view->sortKeys(); !sort.empty())
            {
                std::string name = singleLine(model->columnName(sort.front().column));
                if (string_width(name) > MAX_SORT_STATUS_WIDTH)
                    name = fitText(name, MAX_SORT_STATUS_WIDTH, TextAlign::Left);
                right.push_back(text("│ sort: " + name + (sort.front().ascending ? " ▲ " : " ▼ ")));
            }
            if (const std::string filter = filterStatusText(); !filter.empty())
                right.push_back(text("│ " + filter + " "));
            if (const std::string search = searchStatusText(); !search.empty())
                right.push_back(text("│ " + search + " "));
        }
    }
    if (TuiTableView *header_view = currentView();
        header_view && header_view->model() && header_view->model()->hasHeader() && headerOptionAvailable())
        right.push_back(text("│ header "));
    if (sql_running_)
        right.push_back(text("│ running SQL "));
    if (converting_)
        right.push_back(text("│ " + convert_label_ + ": " + std::to_string(convert_rows_) + " rows (Esc: cancel) "));
    if (skipsMetaLines() && skipMetaLinesAvailable())
        right.push_back(text("│ skip ## "));
    if (TuiTableView *hidden_view = currentView(); hidden_view && !hidden_view->hiddenColumns().empty())
        right.push_back(text("│ " + std::to_string(hidden_view->hiddenColumns().size()) + " hidden (X: show) "));
    if (TuiTableView *frozen_view = currentView(); frozen_view && frozen_view->frozenColumns() > 0)
        right.push_back(text("│ " + std::to_string(frozen_view->frozenColumns()) + " frozen "));
    if (TuiTableView *stats_view = currentView(); stats_view && stats_view->model())
    {
        if (const ColumnStatsState stats = stats_view->model()->columnStatsState(); stats.active && !stats.done)
            right.push_back(text("│ stats: " + std::to_string(stats.scanned_rows) + " rows "));
    }
    if (skipMetaLinesAvailable() && textEncoding() != TextEncoding::Utf8)
        right.push_back(text(std::string("│ ") + textEncodingName(textEncoding()) + " "));
    if (follow_)
        right.push_back(text("│ follow "));
    right.push_back(text(auto_revert_ ? "│ auto-revert " : "│ no auto-revert "));
    right.push_back(text("│ ? help "));

    Element message;
    if (!status_message_.empty())
    {
        message = text(" " + status_message_);
        if (status_is_error_)
            message = message | color(Color::Red);
    }
    else
    {
        // The keys used most, as many as fit between the two sides.
        int used = 0;
        for (const Elements *side : {&left, &right})
        {
            for (const Element &element : *side)
            {
                element->ComputeRequirement();
                used += element->requirement().min_x;
            }
        }
        // Grey text: the bar is inverted, so its background is the text
        // colour. Some terminals (xterm) dim the background of inverted
        // text as well, which would make the hints a grey block.
        message = hbox(text(keyHintText(Terminal::Size().dimx - used)) | bgcolor(Color::GrayDark), filler());
    }
    Elements line = std::move(left);
    line.push_back(message | flex);
    for (auto &element : right)
        line.push_back(std::move(element));
    return hbox(std::move(line)) | inverted;
}

std::string TuiApp::keyHintText(int width) const
{
    TuiTableView *view = currentView();
    if (!book_ || !view || !view->model() || histogram_model_ || density_model_)
        return {};
    std::vector<std::string> hints;
    if (editing())
        hints = {"Enter edit", "u undo", "^R redo", "^S save", "I leave edit mode"};
    else
    {
        hints = {"/ search", "f filter", "o sort", "S stats", "B histogram", "M density", "A group by"};
        if (book_->supportsSql())
            hints.push_back(": SQL");
        hints.push_back("w export");
        if (book_->supportsEditing())
            hints.push_back("I edit");
    }
    hints.push_back("q quit");
    std::string text;
    for (const std::string &hint : hints)
    {
        // Whole hints only, two spaces apart and from the right side.
        if (string_width(text) + string_width(hint) + 4 > width)
            break;
        text += "  " + hint;
    }
    return text;
}

// Replaces the status bar while typing, like vim's command line.
Element TuiApp::renderSearchPrompt()
{
    return hbox({
        text((search_regex_ ? "regex /" : "/") + search_input_),
        text(" ") | inverted, // cursor
        filler(),
        text(std::string("Ctrl+R: regex ") + (search_regex_ ? "on" : "off") + "  Enter: search  Esc: cancel ") | dim,
    });
}

Element TuiApp::renderGoToPrompt()
{
    return hbox({
        text("Go to: " + goto_input_),
        text(" ") | inverted, // cursor
        filler(),
        text("row, B12, R12C2 or column  Enter: go  Esc: cancel ") | dim,
    });
}

Element TuiApp::renderExportPrompt()
{
    const std::string hint = "  .csv .tsv .xlsx .parquet  Enter: export  Esc: cancel ";
    const std::string label = "Export to: ";
    // The end of a long path, where the typing is.
    const int width = std::max(Terminal::Size().dimx - static_cast<int>(hint.size() + label.size()) - 2, 10);
    size_t start = 0;
    while (start < export_input_.size() && string_width(export_input_.substr(start)) > width)
    {
        ++start;
        while (start < export_input_.size() && (static_cast<unsigned char>(export_input_[start]) & 0xC0) == 0x80)
            ++start;
    }
    return hbox({
        text(label + (start > 0 ? "…" : "") + export_input_.substr(start)),
        text(" ") | inverted, // cursor
        filler(),
        text(hint) | dim,
    });
}

Element TuiApp::renderSqlPrompt()
{
    std::string hint = "  Up/Down: history  Tab: complete  Enter: run  Esc: cancel ";
    if (sql_completion_)
    {
        // The candidates Tab goes through, as many as fit in a third of the line.
        hint = "  Tab:";
        const int room = std::max(Terminal::Size().dimx / 3, 20);
        for (const auto &candidate : sql_completion_->candidates)
        {
            if (string_width(hint) + string_width(candidate) + 5 > room)
            {
                hint += " …";
                break;
            }
            hint += " " + candidate;
        }
        hint += " ";
    }
    // The end of a long statement, where the typing is.
    const int width = std::max(Terminal::Size().dimx - static_cast<int>(hint.size()) - 3, 10);
    size_t start = 0;
    if (string_width(promptLine(sql_input_)) > width)
    {
        // Drop whole UTF-8 characters from the front.
        while (start < sql_input_.size() && string_width(promptLine(std::string_view(sql_input_).substr(start))) > width - 1)
        {
            ++start;
            while (start < sql_input_.size() && (static_cast<unsigned char>(sql_input_[start]) & 0xC0) == 0x80)
                ++start;
        }
    }
    Elements line{text(start > 0 ? ":…" : ":")};
    for (const auto &token : tokenizeSql(sql_input_))
    {
        if (token.end <= start)
            continue;
        const size_t begin = std::max(token.begin, start);
        Element part = text(promptLine(std::string_view(sql_input_).substr(begin, token.end - begin)));
        switch (token.kind)
        {
        case SqlTokenKind::Keyword:
            part = part | bold | color(Color::CyanLight);
            break;
        case SqlTokenKind::String:
            part = part | color(Color::YellowLight);
            break;
        case SqlTokenKind::Number:
            part = part | color(Color::GreenLight);
            break;
        case SqlTokenKind::Comment:
            part = part | dim;
            break;
        case SqlTokenKind::Plain:
            break;
        }
        line.push_back(std::move(part));
    }
    line.push_back(text(" ") | inverted); // cursor
    line.push_back(filler());
    line.push_back(text(hint) | dim);
    return hbox(std::move(line));
}

const std::vector<std::string> &TuiApp::dialogRows() const
{
    return dialog_wrap_ ? dialog_rows_ : dialog_lines_;
}

Element TuiApp::renderTextDialog()
{
    const Dimensions terminal = Terminal::Size();
    const int max_width = std::max(terminal.dimx - 8, 20);
    const int max_height = std::max(terminal.dimy - 4, 5);

    // The border takes 2 columns; the border, a separator and the footer take 4 rows.
    int text_width = 40;
    for (const std::string &line : dialog_lines_)
        text_width = std::max(text_width, string_width(line));
    text_width = std::min(text_width, max_width - 2);
    if (dialog_wrap_ && dialog_rows_width_ != text_width)
    {
        dialog_rows_.clear();
        for (const std::string &line : dialog_lines_)
        {
            for (std::string &row : wrapText(line, text_width))
                dialog_rows_.push_back(std::move(row));
        }
        dialog_rows_width_ = text_width;
    }
    const std::vector<std::string> &rows = dialogRows();
    const int text_height = static_cast<int>(std::clamp<size_t>(rows.size(), 1, static_cast<size_t>(max_height - 4)));
    dialog_page_ = text_height;
    dialog_scroll_ = std::min(dialog_scroll_, rows.size() - static_cast<size_t>(text_height));

    Elements lines;
    const size_t end = std::min(rows.size(), dialog_scroll_ + static_cast<size_t>(text_height));
    for (size_t i = dialog_scroll_; i < end; ++i)
        lines.push_back(text(rows[i]));

    std::string position;
    if (rows.size() > static_cast<size_t>(text_height))
        position = " " + std::to_string(dialog_scroll_ + 1) + "-" + std::to_string(end) + "/" + std::to_string(rows.size()) + " ";
    return window(text(" " + dialog_title_ + " ") | bold,
                  vbox({
                      vbox(std::move(lines)) | size(WIDTH, EQUAL, text_width) | size(HEIGHT, EQUAL, text_height),
                      separator(),
                      hbox({text("Esc: close  ↑↓ PgUp PgDn: scroll  y: copy") | dim, filler(), text(position) | dim}),
                  }));
}

Element TuiApp::renderFilterDialog()
{
    TuiTableView *view = currentView();
    const std::shared_ptr<TableModel> model = view ? view->model() : nullptr;
    const Dimensions terminal = Terminal::Size();
    const int width = std::clamp(terminal.dimx - 8, 30, MAX_FILTER_DIALOG_WIDTH);
    const int number_width = 4;
    const int column_width = std::clamp(width / 4, 8, 20);
    const int value_width = std::max(width - number_width - column_width - FILTER_OP_WIDTH - 2, 5);
    const auto field = [](const std::string &content, int field_width, bool focused)
    {
        Element element = text(fitText(content, field_width, TextAlign::Left));
        return focused ? element | inverted : element;
    };

    const std::string match = filter_draft_.combine == Filter::Combine::Or ? "Match: any condition (OR)"
                                                                            : "Match: all conditions (AND)";
    Element match_line = text(" " + match + " ");
    if (filter_line_ == 0)
        match_line = match_line | inverted | focus;

    Elements lines;
    const auto &conditions = filter_draft_.conditions;
    for (size_t i = 0; i < conditions.size(); ++i)
    {
        const FilterCondition &condition = conditions[i];
        const bool current = filter_line_ == i + 1;
        const std::string name = model ? singleLine(model->columnName(condition.column)) : columnLetter(condition.column);
        Element value;
        if (!filterOpTakesValue(condition.op))
            value = text(std::string(static_cast<size_t>(value_width), ' '));
        else if (current && filter_field_ == FilterField::Value)
        {
            // The end of a long value stays visible while typing.
            std::string shown = condition.value;
            while (!shown.empty() && string_width(shown) > value_width - 1)
            {
                size_t first = 1; // the first UTF-8 character
                while (first < shown.size() && (static_cast<unsigned char>(shown[first]) & 0xC0) == 0x80)
                    ++first;
                shown.erase(0, first);
            }
            value = hbox({text(shown), text(" ") | inverted, filler()}) | size(WIDTH, EQUAL, value_width);
        }
        else
            value = field(condition.value, value_width, false);
        Element line = hbox({
            text(fitText(current ? "▶" : std::to_string(i + 1), number_width - 1, TextAlign::Right) + " ") | dim,
            field(name, column_width, current && filter_field_ == FilterField::Column),
            text(" "),
            field(filterOpLabel(condition.op), FILTER_OP_WIDTH, current && filter_field_ == FilterField::Operator),
            text(" "),
            value,
        });
        lines.push_back(current ? line | focus : line);
    }
    Element add_line = text(" + Add condition ");
    lines.push_back(filter_line_ == conditions.size() + 1 ? add_line | inverted | focus : add_line | dim);

    Elements body{match_line, separator(), vbox(std::move(lines)) | vscroll_indicator | frame | flex_shrink, separator()};
    if (model && !model->viewCapabilities().filter)
        body.push_back(text("Still opening (cannot filter yet)") | color(Color::Red));
    body.push_back(text("↑↓: line  Tab: field  ←→: change  Enter: apply  Esc: cancel") | dim);
    body.push_back(text("Ctrl+N: add  Ctrl+D: delete  Ctrl+U: clear the value") | dim);
    return window(text(" Filter ") | bold, vbox(std::move(body)) | size(WIDTH, EQUAL, width)) |
           size(HEIGHT, LESS_THAN, std::max(terminal.dimy - 2, 8));
}

// ===========================================================================
// Group By and column formats
// ===========================================================================

void TuiApp::openGroupDialog()
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return;
    const size_t column_count = view->model()->columnCount();
    group_columns_.clear();
    for (size_t c = 0; c < column_count; ++c)
    {
        if (!view->hiddenColumns().contains(c))
            group_columns_.push_back(c);
    }
    // The choices of last time that this sheet has; the selected columns
    // (or the cursor's, the first time) to group by.
    const auto missing = [this](size_t column)
    { return !std::binary_search(group_columns_.begin(), group_columns_.end(), column); };
    std::erase_if(group_draft_.keys, missing);
    std::erase_if(group_draft_.values, missing);
    const TuiTableView::Selection sel = view->selection();
    if (sel.col_right > sel.col_left || group_draft_.keys.empty())
    {
        group_draft_.keys.clear();
        for (size_t c = sel.col_left; c <= sel.col_right; ++c)
        {
            if (!missing(c))
                group_draft_.keys.push_back(c);
        }
    }
    if (group_draft_.aggregates.empty())
        group_draft_.aggregates = {Aggregate::Sum, Aggregate::Mean};
    const auto cursor = std::lower_bound(group_columns_.begin(), group_columns_.end(), view->cursorColumn());
    group_line_ = static_cast<size_t>(cursor - group_columns_.begin());
    group_aggregate_ = 0;
    show_group_dialog_ = true;
}

bool TuiApp::handleGroupDialogEvent(Event event)
{
    if (event == Event::Custom)
        return false;
    if (event.is_mouse())
        return true;
    const size_t aggregates_line = group_columns_.size();
    const bool on_column = group_line_ < aggregates_line;
    const size_t page = 10;
    if (event == Event::Escape)
        show_group_dialog_ = false;
    else if (event == Event::Return)
    {
        show_group_dialog_ = false;
        startGroupBy();
    }
    else if (event == Event::ArrowUp || event == Event::k)
        group_line_ = group_line_ > 0 ? group_line_ - 1 : 0;
    else if (event == Event::ArrowDown || event == Event::j)
        group_line_ = std::min(group_line_ + 1, aggregates_line);
    else if (event == Event::PageUp)
        group_line_ = group_line_ > page ? group_line_ - page : 0;
    else if (event == Event::PageDown)
        group_line_ = std::min(group_line_ + page, aggregates_line);
    else if (on_column && (event == Event::g || event == Event::Character(' ')))
        toggleColumn(group_draft_.keys, group_columns_[group_line_]);
    else if (on_column && event == Event::a)
        toggleColumn(group_draft_.values, group_columns_[group_line_]);
    else if (!on_column && (event == Event::ArrowLeft || event == Event::h))
        group_aggregate_ = (group_aggregate_ + std::size(AGGREGATES) - 1) % std::size(AGGREGATES);
    else if (!on_column && (event == Event::ArrowRight || event == Event::l))
        group_aggregate_ = (group_aggregate_ + 1) % std::size(AGGREGATES);
    else if (!on_column && event == Event::Character(' '))
    {
        auto &aggregates = group_draft_.aggregates;
        const Aggregate aggregate = AGGREGATES[group_aggregate_];
        if (std::erase(aggregates, aggregate) == 0)
        {
            // In the order of AGGREGATES, which the result's columns follow.
            aggregates.push_back(aggregate);
            std::sort(aggregates.begin(), aggregates.end());
        }
    }
    return true;
}

Element TuiApp::renderGroupDialog()
{
    TuiTableView *view = currentView();
    const std::shared_ptr<TableModel> model = view ? view->model() : nullptr;
    const Dimensions terminal = Terminal::Size();
    const int width = std::clamp(terminal.dimx - 8, 40, MAX_FILTER_DIALOG_WIDTH);
    const std::string flags_header = "  group  aggregate";
    const int name_width = std::max(width - 2 - static_cast<int>(flags_header.size()), 8);
    const auto mark = [](bool on)
    { return std::string(on ? "[x]" : "[ ]"); };

    Elements lines;
    for (size_t i = 0; i < group_columns_.size(); ++i)
    {
        const size_t column = group_columns_[i];
        const bool current = group_line_ == i;
        const auto chosen = [column](const std::vector<size_t> &list)
        { return std::find(list.begin(), list.end(), column) != list.end(); };
        const std::string name = model ? singleLine(model->columnName(column)) : columnLetter(column);
        Element line = hbox({
            text(current ? "▶ " : "  ") | dim,
            text(fitText(name, name_width, TextAlign::Left)),
            text("   " + mark(chosen(group_draft_.keys)) + "     " + mark(chosen(group_draft_.values)) + "   "),
        });
        lines.push_back(current ? line | inverted | focus : line);
    }

    Elements aggregates{text(" Of each: ")};
    for (size_t i = 0; i < std::size(AGGREGATES); ++i)
    {
        const bool on = std::find(group_draft_.aggregates.begin(), group_draft_.aggregates.end(), AGGREGATES[i]) !=
                        group_draft_.aggregates.end();
        Element item = text(mark(on) + " " + aggregateName(AGGREGATES[i]));
        if (group_line_ == group_columns_.size() && group_aggregate_ == i)
            item = item | inverted;
        aggregates.push_back(item);
        aggregates.push_back(text(" "));
    }

    Elements body{
        text("  " + fitText("Column", name_width, TextAlign::Left) + flags_header) | bold,
        vbox(std::move(lines)) | vscroll_indicator | frame | flex_shrink,
        separator(),
        hflow(std::move(aggregates)),
        separator(),
        text("↑↓: line  g, Space: group by the column  a: aggregate it") | dim,
        text("Last line: ←→: choose  Space: on / off    Enter: group  Esc: cancel") | dim,
    };
    return window(text(" Group By ") | bold, vbox(std::move(body)) | size(WIDTH, EQUAL, width)) |
           size(HEIGHT, LESS_THAN, std::max(terminal.dimy - 2, 8));
}

void TuiApp::startGroupBy()
{
    TuiTableView *view = currentView();
    if (converting_ || !view || !view->model())
        return;
    const std::shared_ptr<TableModel> model = view->model();
    const GroupBySpec spec = group_draft_;
    converting_ = true;
    convert_label_ = "grouping";
    convert_rows_ = 0;
    convert_thread_ = std::jthread([this, model, spec, book = std::weak_ptr<TableBook>(book_), post = poster()](std::stop_token stop)
                                   {
        // Shared: posted closures are copied.
        auto sheet = std::make_shared<std::optional<ResultSheet>>();
        std::string error;
        try
        {
            *sheet = groupByView(model, spec, stop, [this, &post](size_t n)
                                 { post([this, n] { convert_rows_ = n; }); });
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        post([this, sheet, book, error]
             {
            converting_ = false;
            if (!error.empty())
            {
                spdlog::error("Failed to group the rows: {}", error);
                showError("Failed to group the rows:\n" + error);
                return;
            }
            if (!*sheet)
            {
                setStatus("Group By canceled");
                return;
            }
            // The file was reopened meanwhile: the rows were the old book's.
            if (!book_ || book.lock() != book_)
                return;
            const size_t groups = (*sheet)->rows.size();
            group_result_ = ResultRows::make(std::move(**sheet));
            group_sheet_ = book_->addResultSheet(GROUP_BY_SHEET, group_result_);
            showResultSheet(group_sheet_);
            setStatus(std::to_string(groups) + " groups in the sheet \"" + group_sheet_ + "\"");
        }); });
}

void TuiApp::showResultSheet(const std::string &sheet)
{
    const auto names = book_->sheetNames();
    const size_t index = static_cast<size_t>(std::find(names.begin(), names.end(), sheet) - names.begin());
    if (index >= names.size())
        return;
    views_.resize(names.size());
    // New rows: a fresh view, not the old position, sort or filter.
    views_[index].reset();
    restore_positions_.erase(sheet);
    restore_views_.erase(sheet);
    restore_stats_.erase(sheet);
    restore_hidden_.erase(sheet);
    restore_frozen_.erase(sheet);
    restore_formats_.erase(sheet);
    showSheet(index);
}

void TuiApp::openFormatDialog()
{
    TuiTableView *view = currentView();
    if (!view || !view->model() || view->model()->columnCount() == 0)
        return;
    format_draft_ = view->columnFormats().get(view->cursorColumn());
    format_line_ = 0;
    show_format_dialog_ = true;
}

bool TuiApp::handleFormatDialogEvent(Event event)
{
    if (event == Event::Custom)
        return false;
    if (event.is_mouse())
        return true;
    TuiTableView *view = currentView();
    if (!view || !view->model())
    {
        show_format_dialog_ = false;
        return true;
    }
    const bool forward = event == Event::ArrowRight || event == Event::l || event == Event::Character(' ');
    const bool backward = event == Event::ArrowLeft || event == Event::h;
    if (event == Event::Escape)
        show_format_dialog_ = false;
    else if (event == Event::Return)
    {
        show_format_dialog_ = false;
        const TuiTableView::Selection sel = view->selection();
        ColumnFormats formats = view->columnFormats();
        for (size_t c = sel.col_left; c <= sel.col_right; ++c)
        {
            if (!view->hiddenColumns().contains(c))
                formats.set(c, format_draft_);
        }
        view->setColumnFormats(formats);
    }
    else if (event == Event::Delete || event == Event::Backspace)
        format_draft_ = {};
    else if (event == Event::ArrowUp || event == Event::k)
        format_line_ = format_line_ > 0 ? format_line_ - 1 : 0;
    else if (event == Event::ArrowDown || event == Event::j || event == Event::Tab)
        format_line_ = std::min<size_t>(format_line_ + 1, 2);
    else if ((forward || backward) && format_line_ == 0)
        format_draft_.thousands = !format_draft_.thousands;
    else if ((forward || backward) && format_line_ == 1)
    {
        // As stored (-1), then 0 to MAX_DECIMALS.
        const int count = MAX_DECIMALS + 2;
        const int index = format_draft_.decimals.value_or(-1) + 1;
        const int next = (index + (forward ? 1 : count - 1)) % count;
        format_draft_.decimals = next == 0 ? std::nullopt : std::optional(next - 1);
    }
    else if ((forward || backward) && format_line_ == 2)
    {
        const size_t count = std::size(DATE_STYLES);
        const auto it = std::find(std::begin(DATE_STYLES), std::end(DATE_STYLES), format_draft_.date);
        const size_t index = static_cast<size_t>(it - std::begin(DATE_STYLES));
        format_draft_.date = DATE_STYLES[(index + (forward ? 1 : count - 1)) % count];
    }
    return true;
}

Element TuiApp::renderFormatDialog()
{
    TuiTableView *view = currentView();
    const std::shared_ptr<TableModel> model = view ? view->model() : nullptr;
    if (!model)
        return text("");
    const Dimensions terminal = Terminal::Size();
    const int width = std::clamp(terminal.dimx - 8, 40, 60);
    const TuiTableView::Selection sel = view->selection();
    const size_t columns = view->hiddenColumns().visibleIn(sel.col_left, sel.col_right);
    const size_t column = view->cursorColumn();
    const std::vector<ColumnType> types = model->columnTypes();
    const std::string title = columns > 1 ? std::to_string(columns) + " columns" : singleLine(model->columnName(column));

    const auto line = [this](size_t index, const std::string &label, const std::string &value)
    {
        Element element = hbox({text(" " + label), filler(), text(value + " ")});
        return format_line_ == index ? element | inverted : element;
    };
    const std::string decimals = format_draft_.decimals ? std::to_string(*format_draft_.decimals) : "as stored";
    Elements body{
        text(" " + fitText(title, width - 4, TextAlign::Left)) | bold,
        text(" Type: " + columnTypeText(column < types.size() ? types[column] : ColumnType{})) | dim,
        separator(),
        line(0, "Thousands separator", format_draft_.thousands ? "[x]" : "[ ]"),
        line(1, "Decimal places", "◂ " + decimals + " ▸"),
        line(2, "Date format", "◂ " + dateStyleExample(format_draft_.date) + " ▸"),
        separator(),
        text("↑↓: line  ←→, Space: change  Del: reset") | dim,
        text("Enter: apply  Esc: cancel (only the display changes)") | dim,
    };
    return window(text(" Format ") | bold, vbox(std::move(body)) | size(WIDTH, EQUAL, width));
}

// ===========================================================================
// Editing
// ===========================================================================

bool TuiApp::editing() const
{
    return book_ && book_->editing();
}

void TuiApp::setEditing(bool editing)
{
    if (!book_)
        return;
    book_->setEditing(editing);
    editing_ = book_->editing();
    setStatus(editing_ ? "Edit mode: Enter / i edits a cell, u undoes, Ctrl+S saves, I leaves" : "Edit mode off");
}

bool TuiApp::requireEditMode()
{
    if (editing())
        return true;
    setStatus(book_ && book_->supportsEditing() ? "Not in edit mode (I: edit mode)"
                                                : "Only CSV, TSV and Excel files can be edited",
              true);
    return false;
}

void TuiApp::editCell()
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    const auto &model = view->model();
    const size_t row = view->cursorRow(), col = view->cursorColumn();
    if (!model->cellEditable(row, col))
    {
        setStatus("This cell cannot be edited (a formula, a time, part of a merged cell, or not read yet)", true);
        return;
    }
    const CellRef cell = model->peek(row, col);
    edit_input_ = cell.state == CellState::Ready ? formatCell(*cell.value) : std::string();
    edit_model_ = model;
    edit_row_ = row;
    edit_col_ = col;
    edit_prompt_ = true;
}

bool TuiApp::handleEditPromptEvent(Event event)
{
    if (event == Event::Return)
        finishEditPrompt(true, 1, 0);
    else if (event == Event::Tab)
        finishEditPrompt(true, 0, 1);
    else if (event == Event::TabReverse)
        finishEditPrompt(true, 0, -1);
    else if (event == Event::Escape)
        finishEditPrompt(false, 0, 0);
    else if (event == Event::Backspace)
        eraseLastCharacter(edit_input_);
    else if (event == Event::CtrlU)
        edit_input_.clear();
    else if (event.is_character())
        edit_input_ += event.character();
    return true;
}

// Keeps (or drops) what was typed and moves the cursor like a spreadsheet.
void TuiApp::finishEditPrompt(bool commit, int rows, int cols)
{
    edit_prompt_ = false;
    const std::shared_ptr<TableModel> model = std::exchange(edit_model_, nullptr);
    if (!commit || !model)
        return;
    model->setCells({{edit_row_, edit_col_, edit_input_}});
    TuiTableView *view = currentView();
    if (!view || view->model() != model || model->displayRowCount() == 0 || model->columnCount() == 0)
        return;
    const size_t last_row = model->displayRowCount() - 1;
    const size_t last_col = model->columnCount() - 1;
    size_t row = edit_row_, col = edit_col_;
    if (rows > 0 && row < last_row)
        ++row;
    if (cols > 0 && col < last_col)
        col = view->hiddenColumns().nearestVisible(col + 1, 1, model->columnCount());
    else if (cols < 0 && col > 0)
        col = view->hiddenColumns().nearestVisible(col - 1, -1, model->columnCount());
    view->jumpTo(row, col);
}

Element TuiApp::renderEditPrompt()
{
    const std::string hint = "  Enter: done  Tab: next column  Esc: cancel ";
    const std::string label = "Edit " + cellName(edit_row_, edit_col_) + ": ";
    const std::string line = promptLine(edit_input_);
    // The end of a long value, where the typing is.
    const int width = std::max(Terminal::Size().dimx - static_cast<int>(hint.size() + label.size()) - 2, 10);
    size_t start = 0;
    while (start < line.size() && string_width(line.substr(start)) > width)
    {
        ++start;
        while (start < line.size() && (static_cast<unsigned char>(line[start]) & 0xC0) == 0x80)
            ++start;
    }
    return hbox({
        text(label + (start > 0 ? "…" : "") + line.substr(start)),
        text(" ") | inverted, // cursor
        filler(),
        text(hint) | dim,
    });
}

void TuiApp::clearSelectedCells()
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    const TuiTableView::Selection sel = view->selection();
    const size_t columns = view->hiddenColumns().visibleIn(sel.col_left, sel.col_right);
    if (columns > 0 && sel.row_bottom - sel.row_top >= MAX_CLEARED_CELLS / columns)
    {
        setStatus("Too many cells to clear at once", true);
        return;
    }
    std::vector<TableModel::CellText> cells;
    for (size_t r = sel.row_top; r <= sel.row_bottom; ++r)
    {
        for (size_t c = sel.col_left; c <= sel.col_right; ++c)
        {
            if (!view->hiddenColumns().contains(c))
                cells.push_back({r, c, {}});
        }
    }
    const size_t changed = view->model()->setCells(cells);
    setStatus(changed > 0 ? "Cleared " + std::to_string(changed) + " cells (u: undo)" : "Nothing to clear");
}

void TuiApp::undoEdit(bool redo)
{
    TuiTableView *view = currentView();
    if (!view || !view->model())
        return;
    CellPosition cell{SIZE_MAX, SIZE_MAX};
    if (!(redo ? view->model()->redo(&cell) : view->model()->undo(&cell)))
    {
        setStatus(redo ? "Nothing to redo" : "Nothing to undo", true);
        return;
    }
    if (cell.row != SIZE_MAX)
        view->jumpTo(cell.row, cell.col);
    setStatus(redo ? "Redone" : "Undone");
}

void TuiApp::pasteCells(const std::string &text)
{
    TuiTableView *view = currentView();
    if (saving_ || !editing() || !view || !view->model() || text.empty())
        return;
    const TuiTableView::Selection sel = view->selection();
    const auto &model = view->model();
    const std::vector<TableModel::CellText> cells =
        pastedCells(parseTsv(text), sel.row_top, sel.col_left, sel.row_bottom, sel.col_right, model->displayRowCount(),
                    model->columnCount(), &view->hiddenColumns());
    const size_t changed = cells.empty() ? 0 : model->setCells(cells);
    setStatus(changed > 0 ? "Pasted into " + std::to_string(changed) + " cells (u: undo)" : "Nothing was pasted",
              changed == 0);
}

// Saving writes a new file on the book's worker; the book is then closed,
// so that the file can be replaced (Windows), and the file opened again.
void TuiApp::save(std::function<void()> then)
{
    // The book is closed after saving, which would stop an export.
    if (!book_ || refuseReopen())
        return;
    saving_ = true;
    setStatus("Saving...");
    const std::filesystem::path target = utf8Path(path_);
    book_->saveEdits(target, [this, post = poster(), book = std::weak_ptr<TableBook>(book_), target,
                              then = std::move(then)](const TableBook::SaveResult &result)
                     { post([this, book, target, result, then]
                            { saveFinished(book.lock(), target, result, then); }); });
}

void TuiApp::saveFinished(std::shared_ptr<TableBook> book, const std::filesystem::path &target,
                          const TableBook::SaveResult &result, const std::function<void()> &then)
{
    saving_ = false;
    if (!book || book != book_)
        return;
    book.reset();
    if (!result.ok)
    {
        spdlog::error("Failed to save {}: {}", utf8String(target), result.message);
        showError("Failed to save " + utf8String(target.filename()) + ":\n" + result.message);
        return;
    }
    closed_sheet_names_ = book_->sheetNames();
    // The views keep showing the rows they read.
    book_.reset();
    const std::string path = path_;
    try
    {
        commitSave(result.written, target);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to replace {}: {}", utf8String(target), e.what());
        if (!open(path))
            dropViews();
        showError("Failed to replace " + utf8String(target) + ":\n" + e.what() +
                  "\n\nThe file with the changes was saved as " + utf8String(result.written) + ".");
        return;
    }
    if (!open(path))
    {
        dropViews();
        return;
    }
    setStatus("Saved " + utf8String(target.filename()));
    if (then)
        then();
}

// The file could not be opened again after it was closed: the views would
// outlive their book, so nothing is shown (r tries again).
void TuiApp::dropViews()
{
    table_holder_->DetachAllChildren();
    views_.clear();
}

void TuiApp::checkUnsaved(std::function<void()> action, bool reload)
{
    if (!book_ || !book_->modified())
    {
        action();
        return;
    }
    unsaved_action_ = std::move(action);
    unsaved_reload_ = reload;
}

bool TuiApp::handleUnsavedPromptEvent(Event event)
{
    const bool yes = event == Event::y || event == Event::Y;
    const bool no = event == Event::n || event == Event::N;
    if (!yes && !no)
    {
        if (event == Event::Escape)
        {
            unsaved_action_ = nullptr;
            setStatus("Canceled");
        }
        return true; // the question stays otherwise
    }
    std::function<void()> action = std::exchange(unsaved_action_, nullptr);
    if (yes && !unsaved_reload_)
    {
        save(std::move(action));
        return true;
    }
    if (book_)
        book_->discardEdits();
    action();
    return true;
}

Element TuiApp::renderUnsavedPrompt()
{
    const std::string question = unsaved_reload_ ? " Discard the unsaved changes? (y/n) "
                                                 : " There are unsaved changes. Save them? (y: save / n: discard) ";
    return hbox({
               text(question) | bold,
               filler(),
               text("Esc: cancel ") | dim,
           }) |
           inverted;
}
