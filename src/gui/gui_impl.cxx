#include "gui.h"
#include "gui_table_view.hxx"
#include "gui_convert.hxx"
#include "gui_help.hxx"
#include "gui_sql.hxx"
#include "table_export.hxx"
#include "file_util.hxx"
#include "gui_theme.hxx"
#include "gui_window_menu.hxx"

#include "spdlog/spdlog.h"
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <climits>
#include <filesystem>
#include <map>

const int TAB_HEIGHT = 25;
// Auto Revert polls instead of watching the file, which keeps it portable and
// works on network drives.
const double AUTO_REVERT_FOCUSED_INTERVAL = 1.0;
const double AUTO_REVERT_UNFOCUSED_INTERVAL = 5.0;
const int SIDEBAR_MIN_WIDTH = 80;
const int TABLE_MIN_WIDTH = 200;

// Returns false (keeping the current file) if the file cannot be read.
// `keep_state`: a file saved from this one (Save As) keeps what the same
// file would keep when reopened.
bool TableWindow::open(const std::string &path, bool report_errors, bool keep_state)
{
    spdlog::debug("Open: {}", path);
    // Taken before reading so that a change made while reading is not missed.
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    const bool has_stamp = readFileStamp(utf8Path(path), mtime, size);
    const bool same_file = path == current_path || keep_state;
    // A choice made with View > Skip lines starting with ## or View > Text
    // Encoding applies to that file only (or, made before any file was
    // opened, to the first one).
    const bool keep_skip_choice = same_file || current_path.empty();
    std::shared_ptr<TableBook> book;
    try
    {
        // Like Excel, the first row is data (columns A, B, C, ...) unless
        // View > Use first row as header is checked.
        OpenOptions options;
        options.header = first_row_as_header_ ? OpenOptions::Header::Yes : OpenOptions::Header::No;
        if (keep_skip_choice)
        {
            options.skip_meta_lines = skip_meta_lines_;
            options.encoding = text_encoding_;
        }
        book = TableBook::open(utf8Path(path), options);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to open {}: {}", path, e.what());
        if (report_errors)
        {
            NestedLoopGuard guard;
            // '@' (in a file name, say) would start a symbol in the label.
            fl_alert("Failed to open %s:\n%s", escapeLabel(path).c_str(), escapeLabel(e.what()).c_str());
        }
        return false;
    }
    if (has_stamp)
    {
        file_mtime_ = mtime;
        file_size_ = size;
    }

    // When the same file is reopened (e.g. the header option changed), stay on the same sheet.
    // Reverting also keeps each sheet's scroll position.
    // Sorts and filters are kept too.
    // By name: sheets may have been added, removed or reordered.
    std::string selected_sheet;
    std::map<std::string, GuiTableView::ScrollPosition> scroll_positions;
    std::map<std::string, ViewSpec> views;
    std::map<std::string, size_t> stats_columns;
    std::map<std::string, HiddenColumns> hidden_columns;
    std::map<std::string, int> frozen_columns;
    if (!keep_skip_choice)
    {
        skip_meta_lines_.reset();
        text_encoding_.reset();
    }
    if (same_file && !gui_table_views.empty())
    {
        // The views' models: the book is gone after saving.
        std::vector<std::string> old_sheet_names;
        for (const auto *table_view : gui_table_views)
            old_sheet_names.push_back(table_view->model() ? table_view->model()->name() : std::string());
        for (size_t i = 0; i < gui_table_views.size() && i < old_sheet_names.size(); ++i)
        {
            if (tab->value() == gui_table_views[i])
                selected_sheet = old_sheet_names[i];
            scroll_positions[old_sheet_names[i]] = gui_table_views[i]->restorableScrollPosition();
            views[old_sheet_names[i]] = gui_table_views[i]->restorableView();
            if (!gui_table_views[i]->hiddenColumns().empty())
                hidden_columns[old_sheet_names[i]] = gui_table_views[i]->hiddenColumns();
            if (gui_table_views[i]->frozenColumns() > 0)
                frozen_columns[old_sheet_names[i]] = gui_table_views[i]->frozenColumns();
            if (const auto &model = gui_table_views[i]->model(); model && model->columnStatsState().active)
                stats_columns[old_sheet_names[i]] = model->columnStatsState().column;
            // The query's TEMP view is gone with the old book: run it again
            // below and restore its tab when the result arrives.
            if (old_sheet_names[i] == sql_sheet_)
            {
                sql_restore_ = {scroll_positions[sql_sheet_], views[sql_sheet_],
                                tab->value() == gui_table_views[i]};
            }
        }
    }
    else
    {
        last_query_.clear();
        sql_sheet_.clear();
        editing_ = false;
    }
    current_path = path;
    changed_on_disk_ = false;

    // Deleting the views unsubscribes them before the old book stops its worker.
    tab->clear();
    gui_table_views.clear();
    table_book = std::move(book);
    // Edit mode stays on when the same file is reopened (e.g. after saving).
    table_book->setEditing(editing_);
    editing_ = table_book->editing();
    // The old book's completion callback is discarded after a reopen.
    if (sql_window_)
    {
        sql_window_->setRunning(false);
        sql_window_->setStatus("");
    }

    tab->begin();
    for (const auto &sheet_name : table_book->sheetNames())
    {
        GuiTableView *table_view = add_sheet_view(sheet_name);
        if (auto it = scroll_positions.find(sheet_name); it != scroll_positions.end())
            table_view->restoreScrollPosition(it->second);
        if (auto it = views.find(sheet_name); it != views.end())
            table_view->restoreView(it->second);
        if (auto it = hidden_columns.find(sheet_name); it != hidden_columns.end())
            table_view->restoreHiddenColumns(it->second);
        if (auto it = frozen_columns.find(sheet_name); it != frozen_columns.end())
            table_view->freezeColumns(it->second);
        if (auto it = stats_columns.find(sheet_name); it != stats_columns.end() && table_view->model())
            table_view->model()->startColumnStats(it->second);
    }
    tab->end();
    spdlog::debug("Opened {} sheet(s) as {} tab(s)", table_book->sheetNames().size(), tab->children());
    if (!gui_table_views.empty())
    {
        const auto &names = table_book->sheetNames();
        const size_t selected = static_cast<size_t>(std::find(names.begin(), names.end(), selected_sheet) - names.begin());
        tab->value(gui_table_views[selected < gui_table_views.size() ? selected : 0]);
        // clear() deleted the old resizable ("No Data"), which makes Fl_Group
        // scale everything, tab bar included. All views share one geometry,
        // so using the first as resizable keeps the tab bar height fixed.
        tab->resizable(gui_table_views.front());
        tab->init_sizes();
    }
    update_title();
    if (sql_window_)
    {
        sql_window_->setFileName(utf8String(utf8Path(path).filename()));
        sql_window_->setSchema({});
        load_sql_schema();
    }
    if (frontTableWindow() == this)
        sync_menu_items();
    requestWindowMenuUpdate();
    current_sheet_changed();
    tab->redraw();
    if (!last_query_.empty() && table_book->supportsSql())
        run_sql(last_query_, true);
    return true;
}

GuiTableView *TableWindow::add_sheet_view(const std::string &sheet_name)
{
    spdlog::debug("Adding tab for sheet: {}", sheet_name);
    auto *table_view = new GuiTableView(tab->x(), tab->y(), tab->w(), tab->h() - TAB_HEIGHT); // space left below: tabs at the bottom
    table_view->copy_label(escapeLabel(sheet_name).c_str());
    table_view->set_use_monospace_font(use_monospace_font_);
    table_view->setSelectionCallback([this, table_view]
                                     {
        if (tab->value() == table_view)
            update_sidebar(); });
    table_view->setUpdateCallback([this, table_view]
                                  {
        update_sheet_label(table_view);
        update_title();
        // Undo and Redo follow the edits.
        if (editing() && frontTableWindow() == this)
        {
            sync_menu_items();
            requestWindowMenuUpdate();
        }
        if (tab->value() == table_view)
        {
            search_result->update();
            update_search_status();
            filter_pane->update();
            stats_pane->update();
            if (histogram_window_)
                histogram_window_->update();
            if (scatter_window_)
                scatter_window_->update();
        } });
    table_view->setFilterColumnCallback([this](size_t column)
                                        { show_filter(column); });
    table_view->setStatsColumnCallback([this](size_t column)
                                       { show_stats(column); });
    table_view->setHistogramColumnCallback([this](size_t column)
                                           { show_histogram(column); });
    table_view->setScatterColumnCallback([this](size_t column)
                                         { show_scatter_plot(column); });
    table_view->setColumnsChangedCallback([this]
                                          {
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate(); });
    table_view->setCellActivateCallback([this]
                                        { show_content(); });
    table_view->setModel(table_book->sheet(sheet_name));
    gui_table_views.push_back(table_view);
    return table_view;
}

namespace
{
    // Carries a result from the book's worker to the main thread.
    struct SqlDone
    {
        std::weak_ptr<TableWindow *> window;
        std::weak_ptr<TableBook> book;
        std::string sql;
        bool rerun = false;
        TableBook::SqlResult result;
    };

    void sqlDoneHandler(void *data)
    {
        std::unique_ptr<SqlDone> done(static_cast<SqlDone *>(data));
        const auto window = done->window.lock();
        const auto book = done->book.lock();
        if (window && book)
            (*window)->sql_finished(book, done->sql, done->rerun, done->result);
    }

    struct SchemaLoaded
    {
        std::weak_ptr<TableWindow *> window;
        std::weak_ptr<TableBook> book;
        std::vector<SqlTable> schema;
    };

    void schemaLoadedHandler(void *data)
    {
        std::unique_ptr<SchemaLoaded> loaded(static_cast<SchemaLoaded *>(data));
        const auto window = loaded->window.lock();
        const auto book = loaded->book.lock();
        if (window && book)
            (*window)->sql_schema_loaded(book, std::move(loaded->schema));
    }
} // namespace

bool TableWindow::sql_available() const
{
    return table_book && table_book->supportsSql();
}

void TableWindow::show_sql()
{
    if (!sql_available())
        return;
    if (!sql_window_)
        sql_window_ = std::make_unique<SqlWindow>([this](const std::string &sql)
                                                  { run_sql(sql, false); });
    sql_window_->show(utf8String(utf8Path(current_path).filename()));
    load_sql_schema();
}

// For completion in the SQL window; again after SQL ran, which may have
// created tables.
void TableWindow::load_sql_schema()
{
    if (!sql_window_ || !sql_available())
        return;
    table_book->loadSqlSchema([window = std::weak_ptr<TableWindow *>(alive_token_),
                               book = std::weak_ptr<TableBook>(table_book)](std::vector<SqlTable> schema)
                              { Fl::awake(schemaLoadedHandler, new SchemaLoaded{window, book, std::move(schema)}); });
}

void TableWindow::sql_schema_loaded(const std::shared_ptr<TableBook> &book, std::vector<SqlTable> schema)
{
    if (book == table_book && sql_window_)
        sql_window_->setSchema(std::move(schema));
}

// `rerun`: the query of the SQL sheet, run again after the file was reopened.
void TableWindow::run_sql(const std::string &sql, bool rerun)
{
    if (!sql_available())
        return;
    if (!rerun && sql_window_)
        sql_window_->setRunning(true);
    table_book->executeSql(sql, [window = std::weak_ptr<TableWindow *>(alive_token_),
                                 book = std::weak_ptr<TableBook>(table_book), sql, rerun](const TableBook::SqlResult &result)
                           { Fl::awake(sqlDoneHandler, new SqlDone{window, book, sql, rerun, result}); });
}

void TableWindow::sql_finished(const std::shared_ptr<TableBook> &book, const std::string &sql, bool rerun,
                               const TableBook::SqlResult &result)
{
    // A result for a book that was replaced meanwhile (Revert) is stale.
    if (book != table_book)
        return;
    if (!rerun && sql_window_)
        sql_window_->setRunning(false);
    load_sql_schema();
    if (!result.ok)
    {
        if (rerun)
        {
            // E.g. a table the query used is gone.
            spdlog::warn("Running the query again failed: {}", result.message);
            last_query_.clear();
            sql_restore_.reset();
            return;
        }
        spdlog::debug("SQL error: {}", result.message);
        if (sql_window_)
            sql_window_->setStatus("Error: " + result.message, true);
        NestedLoopGuard guard;
        fl_alert("SQL error:\n%s", escapeLabel(result.message).c_str());
        return;
    }
    if (result.sheet.empty())
    {
        if (sql_window_)
            sql_window_->setStatus(result.message);
        return;
    }

    last_query_ = sql;
    sql_sheet_ = result.sheet;
    const auto names = table_book->sheetNames();
    const size_t index = static_cast<size_t>(std::find(names.begin(), names.end(), result.sheet) - names.begin());
    GuiTableView *table_view = nullptr;
    if (index < gui_table_views.size())
    {
        // The same sheet with new rows.
        table_view = gui_table_views[index];
        table_view->setModel(table_book->sheet(result.sheet));
        table_view->restoreScrollPosition({});
    }
    else
    {
        tab->begin();
        table_view = add_sheet_view(result.sheet);
        tab->end();
        if (gui_table_views.size() == 1)
        {
            // See open(): the first view keeps the tab bar height fixed.
            tab->resizable(table_view);
        }
        tab->init_sizes();
    }
    bool select = !rerun;
    if (rerun && sql_restore_)
    {
        table_view->restoreScrollPosition(sql_restore_->scroll);
        table_view->restoreView(sql_restore_->view);
        select = sql_restore_->selected;
    }
    sql_restore_.reset();
    if (select)
    {
        tab->value(table_view);
        current_sheet_changed();
    }
    tab->redraw();
    if (!rerun && sql_window_)
        sql_window_->setStatus("The result is shown in the sheet \"" + result.sheet + "\"");
}

void callback_run_sql(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_sql();
}

// All windows share the static menu items, so they show the front window's state.
void TableWindow::sync_menu_items()
{
    menuitem_monospace_font->value(use_monospace_font());
    menuitem_first_row_header->value(use_first_row_as_header());
    if (header_option_available())
        menuitem_first_row_header->activate();
    else
        menuitem_first_row_header->deactivate();
    menuitem_skip_meta_lines->value(skip_meta_lines());
    if (skip_meta_lines_available())
        menuitem_skip_meta_lines->activate();
    else
        menuitem_skip_meta_lines->deactivate();
    const std::pair<Fl_Menu_Item *, TextEncoding> encoding_items[] = {
        {menuitem_encoding_utf8, TextEncoding::Utf8},
        {menuitem_encoding_utf16le, TextEncoding::Utf16LE},
        {menuitem_encoding_utf16be, TextEncoding::Utf16BE},
        {menuitem_encoding_cp932, TextEncoding::Cp932},
    };
    for (const auto &[item, encoding] : encoding_items)
    {
        // Text formats only, like the ## option.
        item->value(skip_meta_lines_available() && text_encoding() == encoding);
        if (skip_meta_lines_available())
            item->activate();
        else
            item->deactivate();
    }
    if (table_book && table_book->isTextFormat())
        menuitem_convert->activate();
    else
        menuitem_convert->deactivate();
    if (table_book)
    {
        menuitem_column_stats->activate();
        menuitem_histogram->activate();
        menuitem_scatter_plot->activate();
    }
    else
    {
        menuitem_column_stats->deactivate();
        menuitem_histogram->deactivate();
        menuitem_scatter_plot->deactivate();
    }
    auto *current_view = dynamic_cast<GuiTableView *>(tab->value());
    const bool has_sheet = current_view && current_view->model();
    // Commands that need an open file or sheet.
    const std::pair<Fl_Menu_Item *, bool> file_items[] = {
        {menuitem_revert, !current_path.empty()},
        {menuitem_show_in_file_browser, !current_path.empty()},
        {menuitem_export_view, has_sheet},
        {menuitem_copy, has_sheet},
        {menuitem_find, has_sheet},
        {menuitem_go_to, has_sheet},
        {menuitem_filter, has_sheet},
        {menuitem_clear_filter, has_sheet},
    };
    for (const auto &[item, active] : file_items)
    {
        if (active)
            item->activate();
        else
            item->deactivate();
    }
    if (current_view && !current_view->hiddenColumns().empty())
        menuitem_show_all_columns->activate();
    else
        menuitem_show_all_columns->deactivate();
    menuitem_freeze_columns->value(current_view && current_view->frozenColumns() > 0);
    if (current_view && current_view->model() && current_view->cols() > 1)
        menuitem_freeze_columns->activate();
    else
        menuitem_freeze_columns->deactivate();
    if (sql_available())
        menuitem_run_sql->activate();
    else
        menuitem_run_sql->deactivate();
    menuitem_auto_revert->value(auto_revert());
    menuitem_show_sidebar->value(show_sidebar());

    const bool can_edit = table_book && table_book->supportsEditing();
    const bool edit_mode = editing();
    const auto &model = current_view ? current_view->model() : nullptr;
    menuitem_edit_mode->value(edit_mode);
    const std::pair<Fl_Menu_Item *, bool> edit_items[] = {
        {menuitem_edit_mode, can_edit},
        {menuitem_save, edit_mode && table_book->modified()},
        {menuitem_save_as, edit_mode},
        {menuitem_undo, edit_mode && model && model->canUndo()},
        {menuitem_redo, edit_mode && model && model->canRedo()},
        {menuitem_paste, edit_mode && model && model->editMode()},
    };
    for (const auto &[item, active] : edit_items)
    {
        if (active)
            item->activate();
        else
            item->deactivate();
    }
}

// Shows the top-left cell of the current sheet's selection.
void TableWindow::update_sidebar()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    const std::string text = table_view ? table_view->topLeftSelectedCellText() : std::string();
    sidebar_buffer->text(text.c_str());
}

void TableWindow::current_sheet_changed()
{
    update_sidebar();
    sync_search();
    sync_filter();
    sync_stats();
    if (histogram_window_)
    {
        auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
        const auto model = table_view ? table_view->model() : nullptr;
        histogram_window_->setModel(model, model ? model->name() : std::string());
    }
    if (scatter_window_)
    {
        auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
        const auto model = table_view ? table_view->model() : nullptr;
        scatter_window_->setModel(model, model ? model->name() : std::string());
    }
    // View > Show All Columns follows the sheet.
    if (frontTableWindow() == this)
        sync_menu_items();
    requestWindowMenuUpdate();
}

// A filtered sheet says so in its tab, since the sidebar may be hidden.
void TableWindow::update_sheet_label(GuiTableView *table_view)
{
    const auto &model = table_view->model();
    if (!model)
        return;
    const std::string label = escapeLabel(model->name()) + (model->readingStopped() ? " (partly read)" : "") +
                              (table_view->rowsCut() ? " (too many rows to show all)" : "") +
                              (model->view().filtered() ? " (filtered)" : "");
    if (!table_view->label() || label != table_view->label())
    {
        table_view->copy_label(label.c_str());
        tab->redraw();
    }
}

void TableWindow::setup_filter()
{
    // New conditions are on the selected column.
    filter_pane->setDefaultColumn([this]() -> size_t
                                  {
        auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
        if (!table_view)
            return 0;
        int row_top, col_left, row_bottom, col_right;
        table_view->get_selection(row_top, col_left, row_bottom, col_right);
        return col_left >= 0 ? static_cast<size_t>(col_left) : 0; });
}

void TableWindow::sync_filter()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    filter_pane->setModel(table_view ? table_view->model() : nullptr);
}

// Shows the whole top-left selected cell in the sidebar's Content tab.
void TableWindow::show_content()
{
    set_show_sidebar(true);
    sidebar_tab->value(content_pane);
    update_sidebar();
    sync_menu_items();
}

// With a column, adds a condition on it; otherwise starts with one on the
// selected column if there is none.
void TableWindow::show_filter(std::optional<size_t> column)
{
    set_show_sidebar(true);
    sidebar_tab->value(filter_pane);
    sync_filter();
    if (column)
        filter_pane->addCondition(*column);
    else if (!filter_pane->hasConditions())
        filter_pane->addDefaultCondition();
    sync_menu_items();
    requestWindowMenuUpdate();
}

void TableWindow::sync_stats()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    stats_pane->setModel(table_view ? table_view->model() : nullptr);
}

// Without a column, shows the statistics of the selected one.
void TableWindow::show_stats(std::optional<size_t> column)
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (!table_view || !table_view->model())
        return;
    if (!column)
    {
        int row_top, col_left, row_bottom, col_right;
        table_view->get_selection(row_top, col_left, row_bottom, col_right);
        column = col_left >= 0 ? static_cast<size_t>(col_left) : 0;
    }
    set_show_sidebar(true);
    sidebar_tab->value(stats_pane);
    sync_stats();
    stats_pane->showColumn(*column);
    sync_menu_items();
    requestWindowMenuUpdate();
}

void callback_show_all_columns(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        if (auto *table_view = dynamic_cast<GuiTableView *>(self->tab->value()))
            table_view->showAllColumns();
    }
}

// Freezes the columns up to the selection's right end, or unfreezes them.
void callback_freeze_columns(Fl_Menu_ *, void *)
{
    auto *self = frontTableWindow();
    auto *table_view = self ? dynamic_cast<GuiTableView *>(self->tab->value()) : nullptr;
    if (!table_view)
        return;
    if (table_view->frozenColumns() > 0)
    {
        table_view->freezeColumns(0);
        return;
    }
    int row_top, col_left, row_bottom, col_right;
    table_view->get_selection(row_top, col_left, row_bottom, col_right);
    // Without a selection, the first column; one is always left to scroll.
    const int last = std::clamp(col_right, 0, std::max(table_view->cols() - 2, 0));
    table_view->freezeColumns(last + 1);
    // The menu item toggled itself; the view may have refused.
    self->sync_menu_items();
    requestWindowMenuUpdate();
}

void callback_column_stats(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_stats();
}

// Without a column, the histogram of the selected one.
void TableWindow::show_histogram(std::optional<size_t> column)
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (!table_view || !table_view->model())
        return;
    if (!column)
    {
        int row_top, col_left, row_bottom, col_right;
        table_view->get_selection(row_top, col_left, row_bottom, col_right);
        column = col_left >= 0 ? static_cast<size_t>(col_left) : 0;
    }
    if (!histogram_window_)
        histogram_window_ = std::make_unique<HistogramWindow>();
    histogram_window_->show(table_view->model(), table_view->model()->name(), *column);
}

void callback_histogram(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_histogram();
}

// The selection's left and right columns, else columns holding numbers or
// dates near the one given (or the current one), see defaultScatterColumns().
void TableWindow::show_scatter_plot(std::optional<size_t> column)
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (!table_view || !table_view->model())
        return;
    int row_top, col_left, row_bottom, col_right;
    table_view->get_selection(row_top, col_left, row_bottom, col_right);
    size_t x_column = 0;
    size_t y_column = 0;
    if (!column && col_left >= 0 && col_right > col_left)
    {
        x_column = static_cast<size_t>(col_left);
        y_column = static_cast<size_t>(col_right);
    }
    else
    {
        // Judged by the rows around the current one, which are loaded.
        const auto row = static_cast<size_t>(std::max(row_top, 0));
        const ScatterColumns columns = defaultScatterColumns(
            *table_view->model(), column ? *column : static_cast<size_t>(std::max(col_left, 0)),
            table_view->hiddenColumns(), row > 50 ? row - 50 : 0, row + 50);
        x_column = columns.x;
        y_column = columns.y;
    }
    if (!scatter_window_)
    {
        scatter_window_ = std::make_unique<ScatterWindow>();
        scatter_window_->setRowCallback([this](const std::shared_ptr<TableModel> &model, size_t view_row, size_t col)
                                        {
            auto *current = dynamic_cast<GuiTableView *>(tab->value());
            if (current && current->model() == model && view_row <= INT_MAX && col <= INT_MAX)
                current->jumpTo(static_cast<int>(view_row), static_cast<int>(col)); });
    }
    scatter_window_->show(table_view->model(), table_view->model()->name(), x_column, y_column);
}

void callback_scatter_plot(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_scatter_plot();
}

void TableWindow::clear_filter()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (!table_view || !table_view->model())
        return;
    ViewSpec spec = table_view->model()->requestedView();
    spec.filter.reset();
    table_view->model()->setView(std::move(spec));
}

void TableWindow::setup_search()
{
    // Not FLUID callbacks: those find this object through a fixed number of
    // parents, which changes while the sidebar is hidden (see set_show_sidebar()).
    auto start = [](Fl_Widget *, void *data)
    { static_cast<TableWindow *>(data)->start_search(); };
    search_text->callback(start, this);
    search_button->callback(start, this);
    search_regex->callback(start, this);
    search_result->setActivateCallback([this](CellPosition hit)
                                       {
        auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
        if (table_view && table_view->model() == search_result->model() && hit.row <= INT_MAX && hit.col <= INT_MAX)
            table_view->jumpTo(static_cast<int>(hit.row), static_cast<int>(hit.col)); });
}

// Only the current sheet is searched; the others search again when shown.
void TableWindow::start_search()
{
    search_spec_ = {.text = search_text->value(), .regex = search_regex->value() != 0};
    auto *current = dynamic_cast<GuiTableView *>(tab->value());
    for (auto *table_view : gui_table_views)
    {
        if (table_view != current && table_view->model())
            table_view->model()->cancelSearch();
    }
    if (current && current->model())
        current->model()->startSearch(search_spec_);
    for (auto *table_view : gui_table_views)
        table_view->updateSearchHits();
    search_result->setModel(current ? current->model() : nullptr);
    update_search_status();
}

// Points the result list at the current sheet, starting its search if it has
// none yet (another sheet was searched, or the file was reopened).
void TableWindow::sync_search()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    std::shared_ptr<TableModel> model = table_view ? table_view->model() : nullptr;
    if (model && !search_spec_.text.empty() && !model->searchState().active)
        model->startSearch(search_spec_);
    if (search_result->model() != model)
        search_result->setModel(std::move(model));
    else
        search_result->update();
    update_search_status();
}

void TableWindow::update_search_status()
{
    // The text may echo the user's pattern (e.g. an invalid regex).
    const std::string text = escapeLabel(search_result->statusText());
    if (!search_status->label() || text != search_status->label())
    {
        search_status->copy_label(text.c_str());
        // A narrow sidebar cuts long messages; the tooltip shows them whole.
        search_status_tooltip_.set(search_status, text);
    }
}

void TableWindow::show_search()
{
    set_show_sidebar(true);
    sidebar_tab->value(search_pane);
    search_text->take_focus();
    search_text->insert_position(0, search_text->size());
    sync_menu_items();
    requestWindowMenuUpdate();
}

// Asks for a row, cell or column of the current sheet and selects it.
void TableWindow::go_to()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (!table_view || !table_view->model())
        return;
    const char *input;
    {
        NestedLoopGuard guard;
        input = fl_input("Go to a row (120), a cell (B12 or R12C2) or a column (its name or letter):",
                         go_to_text_.c_str());
    }
    if (!input)
        return;
    go_to_text_ = input;
    const GoToResult result = parseGoTo(go_to_text_, *table_view->model());
    if (!result.target)
    {
        NestedLoopGuard guard;
        fl_alert("%s", escapeLabel(result.error).c_str());
        return;
    }
    // A part not given keeps the current cell's row or column.
    int row_top, col_left, row_bottom, col_right;
    table_view->get_selection(row_top, col_left, row_bottom, col_right);
    const size_t row = result.target->row.value_or(static_cast<size_t>(std::max(row_top, 0)));
    const size_t col = result.target->column.value_or(static_cast<size_t>(std::max(col_left, 0)));
    table_view->jumpTo(static_cast<int>(std::min<size_t>(row, INT_MAX)), static_cast<int>(std::min<size_t>(col, INT_MAX)));
    table_view->take_focus();
}

void callback_go_to(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->go_to();
}

const std::string &TableWindow::opened_path() const
{
    return current_path;
}

namespace
{
    void chooseAndOpenFile()
    {
        Fl_Native_File_Chooser chooser;
        chooser.title("Open File");
        chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
        chooser.filter("All Supported Files\t*.{csv,tsv,tab,txt,vcf,xlsx,xlsm,parquet,json,jsonl,ndjson,duckdb,ddb,sqlite,sqlite3,db,db3,gz,bgz,bz2,xz,zst}\nCSV Files\t*.{csv,csv.gz,csv.bz2,csv.xz,csv.zst}\nTSV Files\t*.{tsv,tab,tsv.gz,tsv.bz2,tsv.xz,tsv.zst}\nVCF Files\t*.{vcf,vcf.gz,vcf.bgz}\nExcel Files\t*.{xlsx,xlsm}\nParquet Files\t*.parquet\nJSON Files\t*.{json,jsonl,ndjson,json.gz,jsonl.gz,ndjson.gz,json.zst,jsonl.zst,ndjson.zst}\nDuckDB Files\t*.{duckdb,ddb}\nSQLite Files\t*.{sqlite,sqlite3,db,db3}\nAll Files\t*");
        int chosen;
        {
            NestedLoopGuard guard;
            chosen = chooser.show();
        }
        switch (chosen)
        {
        case -1:
            spdlog::error("File open error: {}", chooser.errmsg());
            break;
        case 1:
            spdlog::debug("File open canceled");
            break;
        default:
            spdlog::debug("Selected file: {}", chooser.filename());
            if (auto *self = frontTableWindow())
            {
                if (self->current_path.empty())
                {
                    self->open(chooser.filename());
                }
                else
                {
                    // If a file is already open, open the new file in a new window.
                    // Register it first: on macOS show() sends FL_FOCUS right away,
                    // and my_dispatch() only syncs the menu for known windows.
                    table_windows.push_back(std::make_unique<TableWindow>());
                    TableWindow *new_window = table_windows.back().get();
                    new_window->show();
                    new_window->open(chooser.filename());
                }
            }
            else
            {
                spdlog::error("No TableWindow to apply the menu item to");
            }
            break;
        }
    }
} // namespace

void open_callback2(Fl_Button *widget, void *userdata)
{
    open_callback(nullptr, userdata);
}

void open_callback(Fl_Menu_ *widget, void *userdata)
{
    // An empty front window opens the file itself, replacing its contents.
    NestedLoopGuard::defer(chooseAndOpenFile);
}

void callback_close(Fl_Menu_ *, void *)
{
    // Saving first reopens the file.
    if (auto *self = frontTableWindow())
        self->defer_command([](TableWindow &window)
                            { window.close_window(); });
}

// For commands that reopen the file, which deletes the views: skipped if the
// window is closed by the time no nested loop is left (see NestedLoopGuard).
void TableWindow::defer_command(std::function<void(TableWindow &)> command)
{
    NestedLoopGuard::defer([window = std::weak_ptr<TableWindow *>(alive_token_), command = std::move(command)]
                           {
        const auto alive = window.lock();
        if (alive && (*alive)->main_window->shown())
            command(**alive); });
}

void callback_revert(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        self->defer_command([](TableWindow &window)
                            {
            if (window.opened_path().empty() || !window.confirm_unsaved(true))
                return;
            window.open(window.opened_path()); });
    }
}

void TableWindow::revert_if_changed()
{
    // Checked before the stamp is read so that the change is picked up by a
    // later poll once the menu or alert is closed. Fl::grab() covers menus
    // that pop up without a guard (menu bar, Fl_Choice).
    if (current_path.empty() || NestedLoopGuard::active() || Fl::grab())
        return;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    // A missing or empty file is usually being replaced or rewritten; wait
    // until it has content again.
    if (!readFileStamp(utf8Path(current_path), mtime, size) || size == 0 || (mtime == file_mtime_ && size == file_size_))
        return;
    if (table_book && table_book->modified())
    {
        // Reopening would lose the edits; the title tells, and saving fails.
        if (!changed_on_disk_)
        {
            spdlog::debug("Auto revert: {} changed, but has unsaved edits", current_path);
            changed_on_disk_ = true;
            update_title();
        }
        return;
    }
    spdlog::debug("Auto revert: {} changed", current_path);
    // Remember the new stamp even if reading fails (e.g. a half-written file),
    // so that a broken file is not retried on every poll.
    file_mtime_ = mtime;
    file_size_ = size;
    // Copy: open() assigns current_path.
    const std::string path = current_path;
    open(path, false);
}

void TableWindow::schedule_auto_revert()
{
    Fl::remove_timeout(auto_revert_timeout, this);
    if (auto_revert_ && main_window->shown())
        Fl::add_timeout(focused_ ? AUTO_REVERT_FOCUSED_INTERVAL : AUTO_REVERT_UNFOCUSED_INTERVAL, auto_revert_timeout, this);
}

void TableWindow::auto_revert_timeout(void *data)
{
    auto *self = static_cast<TableWindow *>(data);
    self->revert_if_changed();
    self->schedule_auto_revert();
}

void TableWindow::set_auto_revert(bool enabled)
{
    auto_revert_ = enabled;
    schedule_auto_revert();
}

void TableWindow::set_focused(bool focused)
{
    focused_ = focused;
    if (focused && auto_revert_)
        revert_if_changed();
    schedule_auto_revert();
}

void callback_auto_revert(Fl_Menu_ *w, void *)
{
    if (auto *self = frontTableWindow())
    {
        const Fl_Menu_Item *item = w->mvalue();
        self->set_auto_revert(item && item->value() != 0);
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

void TableWindow::set_use_monospace_font(bool use)
{
    use_monospace_font_ = use;
    for (const auto &table_view : gui_table_views)
    {
        table_view->set_use_monospace_font(use);
    }
    sidebar_text->textfont(use ? FL_COURIER : FL_HELVETICA);
    sidebar_text->redraw();
}

void callback_use_monospace_font(Fl_Menu_ *w, void *userdata)
{

    if (auto *self = frontTableWindow())
    {
        const Fl_Menu_Item *item = w->mvalue();
        bool use_monospace = item && item->value() != 0;
        self->set_use_monospace_font(use_monospace);
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

void TableWindow::set_first_row_as_header(bool use)
{
    if (first_row_as_header_ == use)
        return;
    // Reading the file again loses the edits.
    if (!confirm_unsaved())
    {
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate();
        return;
    }
    first_row_as_header_ = use;
    // Whether the first row is a header is decided when the file is read.
    // If that fails, the shown file still uses the old setting; keep it so
    // that the menu and a later Auto Revert match what is shown.
    if (!current_path.empty() && header_option_available() && !open(current_path))
    {
        first_row_as_header_ = !use;
        // Deferred (see defer_command()), this may run behind another window.
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate();
    }
}

void callback_use_first_row_as_header(Fl_Menu_ *w, void *userdata)
{
    if (auto *self = frontTableWindow())
    {
        const Fl_Menu_Item *item = w->mvalue();
        const bool use = item && item->value() != 0;
        self->defer_command([use](TableWindow &window)
                            { window.set_first_row_as_header(use); });
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

void TableWindow::set_skip_meta_lines(bool skip)
{
    if (skip_meta_lines() == skip)
        return;
    if (!confirm_unsaved())
    {
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate();
        return;
    }
    const std::optional<bool> old = skip_meta_lines_;
    skip_meta_lines_ = skip;
    // Same as the header option: keep the old setting if the file cannot be reread.
    if (!current_path.empty() && skip_meta_lines_available() && !open(current_path))
    {
        skip_meta_lines_ = old;
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate();
    }
}

void TableWindow::set_text_encoding(TextEncoding encoding)
{
    if (table_book && table_book->textEncoding() == encoding)
        return;
    if (!confirm_unsaved())
    {
        if (frontTableWindow() == this)
            sync_menu_items();
        requestWindowMenuUpdate();
        return;
    }
    const std::optional<TextEncoding> old = text_encoding_;
    text_encoding_ = encoding;
    // Same as the header option: keep the old setting if the file cannot be reread.
    if (!current_path.empty() && skip_meta_lines_available() && !open(current_path))
        text_encoding_ = old;
    // Radio items change their value when clicked, even if nothing was reopened.
    if (frontTableWindow() == this)
        sync_menu_items();
    requestWindowMenuUpdate();
}

// Converts the shown text file and opens the result in this window. The
// format follows the extension of the chosen name, else the chosen filter.
void TableWindow::convert()
{
    if (current_path.empty() || !table_book || !table_book->isTextFormat())
        return;
    // In the order of the filters.
    static const ExportFormat FILTER_FORMATS[] = {ExportFormat::Parquet, ExportFormat::DuckDb};
    const std::filesystem::path source = utf8Path(current_path);
    const std::filesystem::path suggested = defaultExportPath(source, FILTER_FORMATS[0]);
    const std::string directory = utf8String(source.parent_path());
    const std::string file_name = utf8String(suggested.filename());

    Fl_Native_File_Chooser chooser;
    chooser.title("Convert to Parquet / DuckDB");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    chooser.filter("Parquet Files\t*.parquet\nDuckDB Files\t*.{duckdb,ddb}");
    if (!directory.empty())
        chooser.directory(directory.c_str());
    chooser.preset_file(file_name.c_str());
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
    std::optional<ExportFormat> format = exportFormatFromPath(target);
    if (!format)
    {
        const int filter = chooser.filter_value();
        format = FILTER_FORMATS[filter == 1 ? 1 : 0];
        // Not checked by SAVEAS_CONFIRM, which saw the name without it.
        target += exportFormatExtension(*format);
        std::error_code exists_error;
        if (std::filesystem::exists(target, exists_error))
        {
            NestedLoopGuard guard;
            const std::string name = escapeLabel(utf8String(target.filename()));
            if (fl_choice("%s already exists. Replace it?", "Cancel", "Replace", nullptr, name.c_str()) != 1)
                return;
        }
    }
    std::error_code ec;
    if (std::filesystem::equivalent(source, target, ec))
    {
        NestedLoopGuard guard;
        fl_alert("Choose a file other than the one being converted.");
        return;
    }
    // It reads the file, not what is shown.
    if (table_book->modified())
    {
        NestedLoopGuard guard;
        if (fl_choice("The unsaved changes are not converted: the file is converted as it was saved.", "Cancel",
                      "Convert", nullptr) != 1)
            return;
    }

    // Convert what is shown: the same header, ## and encoding choices.
    OpenOptions options;
    options.header = first_row_as_header_ ? OpenOptions::Header::Yes : OpenOptions::Header::No;
    options.skip_meta_lines = table_book->skipsMetaLines();
    options.encoding = table_book->textEncoding();
    if (runConversion(source, target, *format, options))
        open(utf8String(target));
}

void callback_convert(Fl_Menu_ *, void *)
{
    // It opens the result.
    if (auto *self = frontTableWindow())
        self->defer_command([](TableWindow &window)
                            { window.convert(); });
}

// Writes the rows of the current sheet as sorted and filtered, without its
// hidden columns. The format follows the extension of the chosen name, else
// the chosen filter.
void TableWindow::export_view()
{
    auto *table_view = dynamic_cast<GuiTableView *>(tab->value());
    if (current_path.empty() || !table_book || !table_view || !table_view->model())
        return;
    const std::shared_ptr<TableModel> model = table_view->model();
    // In the order of the filters; the last one chosen comes first next time.
    static int last_filter = 0;
    const std::filesystem::path source = utf8Path(current_path);
    const std::string sheet = table_book->sheetNames().size() > 1 ? model->name() : std::string();
    const std::filesystem::path suggested = defaultViewExportPath(source, sheet, VIEW_EXPORT_FORMATS[last_filter]);
    const std::string directory = utf8String(source.parent_path());
    const std::string file_name = utf8String(suggested.filename());

    Fl_Native_File_Chooser chooser;
    chooser.title("Export View");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    chooser.filter("CSV Files\t*.csv\nTSV Files\t*.{tsv,tab}\nExcel Workbooks\t*.xlsx\nParquet Files\t*.parquet");
    chooser.filter_value(last_filter);
    if (!directory.empty())
        chooser.directory(directory.c_str());
    chooser.preset_file(file_name.c_str());
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
    std::optional<ViewExportFormat> format = viewExportFormatFromPath(target);
    if (!format)
    {
        const int filter = chooser.filter_value();
        format = VIEW_EXPORT_FORMATS[filter >= 0 && filter < static_cast<int>(std::size(VIEW_EXPORT_FORMATS)) ? filter : 0];
        // Not checked by SAVEAS_CONFIRM, which saw the name without it.
        target += viewExportFormatExtension(*format);
        std::error_code exists_error;
        if (std::filesystem::exists(target, exists_error))
        {
            NestedLoopGuard guard;
            const std::string name = escapeLabel(utf8String(target.filename()));
            if (fl_choice("%s already exists. Replace it?", "Cancel", "Replace", nullptr, name.c_str()) != 1)
                return;
        }
    }
    last_filter = static_cast<int>(std::find(std::begin(VIEW_EXPORT_FORMATS), std::end(VIEW_EXPORT_FORMATS), *format) -
                                   std::begin(VIEW_EXPORT_FORMATS));

    ViewExportOptions options;
    options.hidden = table_view->hiddenColumns();
    options.source = source;
    runViewExport(model, target, *format, options);
}

void callback_export_view(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->export_view();
}

void callback_skip_meta_lines(Fl_Menu_ *w, void *userdata)
{
    if (auto *self = frontTableWindow())
    {
        const Fl_Menu_Item *item = w->mvalue();
        const bool skip = item && item->value() != 0;
        self->defer_command([skip](TableWindow &window)
                            { window.set_skip_meta_lines(skip); });
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

void callback_text_encoding(Fl_Menu_ *, void *userdata)
{
    const auto encoding = parseTextEncoding(static_cast<const char *>(userdata));
    if (auto *self = frontTableWindow(); self && encoding)
        self->defer_command([encoding = *encoding](TableWindow &window)
                            { window.set_text_encoding(encoding); });
    else
        spdlog::error("No TableWindow to apply the menu item to");
}

void TableWindow::set_show_sidebar(bool show)
{
    if (show == show_sidebar())
        return;
    const int x = tile->x(), y = tile->y(), w = tile->w(), h = tile->h();
    if (show)
    {
        const int width = std::clamp(sidebar_width_, SIDEBAR_MIN_WIDTH, std::max(SIDEBAR_MIN_WIDTH, w - TABLE_MIN_WIDTH));
        tab->resize(x, y, w - width, h);
        sidebar_tab->resize(x + w - width, y, width, h);
        tile->add(sidebar_tab);
        tile->size_range(sidebar_tab, SIDEBAR_MIN_WIDTH, 50);
        sidebar_tab->show();
    }
    else
    {
        // Fl_Tile lays out hidden children too, so the sidebar leaves the tile
        // instead; parking it in the window keeps it owned (and deleted) there.
        sidebar_width_ = sidebar_tab->w();
        sidebar_tab->hide();
        main_window->add(sidebar_tab);
        tab->resize(x, y, w, h);
    }
    tile->init_sizes();
    main_window->redraw();
}

void callback_show_sidebar(Fl_Menu_ *w, void *)
{
    if (auto *self = frontTableWindow())
    {
        const Fl_Menu_Item *item = w->mvalue();
        self->set_show_sidebar(item && item->value() != 0);
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

void show_file_properties(Fl_Menu_ *, void *)
{
}

void show_in_file_browser(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        openWithFileBrowser(utf8Path(self->opened_path()));
    }
}

void duplicate_window(Fl_Menu_ *, void *)
{
}

void callback_show_about(Fl_Menu_ *, void *)
{
    version_window->show();
}

void callback_user_manual(Fl_Menu_ *, void *)
{
    showHelpWindow();
}

void callback_find(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_search();
}

void callback_filter(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->show_filter();
}

void callback_clear_filter(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->clear_filter();
}

void callback_copy_menu(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        if (auto *table_view = dynamic_cast<GuiTableView *>(self->tab->value()))
            table_view->copySelection();
    }
    else
    {
        spdlog::error("No TableWindow to apply the menu item to");
    }
}

// ===========================================================================
// Editing
// ===========================================================================

bool TableWindow::editing() const
{
    return table_book && table_book->editing();
}

void TableWindow::finish_cell_editing()
{
    for (auto *table_view : gui_table_views)
        table_view->finishEditing(true);
}

// "* name [Editing]": unsaved changes and edit mode.
void TableWindow::update_title()
{
    if (current_path.empty())
        return;
    std::string title = utf8String(utf8Path(current_path).filename());
    if (table_book && table_book->modified())
        title = "* " + title;
    if (editing())
        title += " [Editing]";
    if (changed_on_disk_)
        title += " (changed on disk)";
    if (!main_window->label() || title != main_window->label())
    {
        main_window->copy_label(title.c_str());
        requestWindowMenuUpdate();
    }
}

void TableWindow::set_editing(bool editing)
{
    if (!table_book || !table_book->supportsEditing())
        editing = false;
    if (editing != this->editing())
    {
        // Saving reopens the file, which keeps edit mode as editing_ says.
        editing_ = editing;
        if (!editing && !confirm_unsaved())
            editing_ = true;
        else if (table_book)
        {
            if (!editing)
                finish_cell_editing();
            table_book->setEditing(editing_);
        }
        for (auto *table_view : gui_table_views)
            table_view->redraw();
        update_title();
    }
    // The menu item toggled itself.
    if (frontTableWindow() == this)
        sync_menu_items();
    requestWindowMenuUpdate();
}

// Asks what to do with unsaved changes before they would be lost. True if
// they were saved or dropped (or there were none); false to cancel.
// `reload`: the file is read again, so saving first makes no sense.
bool TableWindow::confirm_unsaved(bool reload)
{
    finish_cell_editing();
    if (!table_book || !table_book->modified())
        return true;
    const std::string name = escapeLabel(utf8String(utf8Path(current_path).filename()));
    main_window->show();
    int choice;
    {
        NestedLoopGuard guard;
        if (reload)
            choice = fl_choice("Discard the unsaved changes to %s?", "Cancel", nullptr, "Discard", name.c_str());
        else
            choice = fl_choice("%s has unsaved changes.", "Cancel", "Save", "Don't Save", name.c_str());
    }
    if (choice == 1)
        return save(false);
    if (choice == 2)
    {
        table_book->discardEdits();
        update_title();
        return true;
    }
    return false;
}

bool TableWindow::close_window()
{
    if (!confirm_unsaved())
        return false;
    hide();
    return true;
}

// Writes the edits to the file (or, Save As, to another one in the same
// format) and opens it again. True if saved.
bool TableWindow::save(bool save_as)
{
    finish_cell_editing();
    if (current_path.empty() || !editing())
        return false;
    const std::filesystem::path source = utf8Path(current_path);
    std::filesystem::path target = source;
    if (save_as)
    {
        Fl_Native_File_Chooser chooser;
        chooser.title("Save As");
        chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
        const std::string directory = utf8String(source.parent_path());
        if (!directory.empty())
            chooser.directory(directory.c_str());
        chooser.preset_file(utf8String(source.filename()).c_str());
        int chosen;
        {
            NestedLoopGuard guard;
            chosen = chooser.show();
        }
        if (chosen == -1)
            spdlog::error("File chooser error: {}", chooser.errmsg());
        if (chosen != 0 || !chooser.filename())
            return false;
        target = utf8Path(chooser.filename());
    }

    const TableBook::SaveResult result = runSave(*table_book, target);
    if (!result.ok)
    {
        spdlog::error("Failed to save {}: {}", utf8String(target), result.message);
        NestedLoopGuard guard;
        fl_alert("Failed to save %s:\n%s", escapeLabel(utf8String(target.filename())).c_str(),
                 escapeLabel(result.message).c_str());
        return false;
    }
    // On Windows a file that is open cannot be replaced, so the book goes
    // first; the views keep showing the rows they read.
    table_book.reset();
    try
    {
        commitSave(result.written, target);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to replace {}: {}", utf8String(target), e.what());
        {
            NestedLoopGuard guard;
            fl_alert("Failed to replace %s:\n%s\n\nThe file with the changes was saved as %s.",
                     escapeLabel(utf8String(target)).c_str(), escapeLabel(e.what()).c_str(),
                     escapeLabel(utf8String(result.written)).c_str());
        }
        open(current_path, true, true);
        return false;
    }
    open(utf8String(target), true, true);
    return true;
}

void callback_save(Fl_Menu_ *, void *)
{
    // It reopens the file.
    if (auto *self = frontTableWindow())
        self->defer_command([](TableWindow &window)
                            { window.save(false); });
}

void callback_save_as(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
        self->defer_command([](TableWindow &window)
                            { window.save(true); });
}

void callback_edit_mode(Fl_Menu_ *w, void *)
{
    const Fl_Menu_Item *item = w->mvalue();
    const bool editing = item && item->value() != 0;
    // Leaving it may save, which reopens the file.
    if (auto *self = frontTableWindow())
        self->defer_command([editing](TableWindow &window)
                            { window.set_editing(editing); });
}

void callback_undo(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        if (auto *table_view = dynamic_cast<GuiTableView *>(self->tab->value()))
            table_view->undo();
    }
}

void callback_redo(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        if (auto *table_view = dynamic_cast<GuiTableView *>(self->tab->value()))
            table_view->redo();
    }
}

void callback_paste(Fl_Menu_ *, void *)
{
    if (auto *self = frontTableWindow())
    {
        if (auto *table_view = dynamic_cast<GuiTableView *>(self->tab->value()))
            table_view->paste();
    }
}
