#ifndef _TUI_APP_HXX_
#define _TUI_APP_HXX_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>

#include "table.hxx"
#include "sql_history.hxx"
#include "sql_text.hxx"
#include "table_export.hxx"
#include "tui_table_view.hxx"

// The whole terminal UI: one file at a time, one tab per sheet.
// Everything here runs on the UI (FTXUI loop) thread.
class TuiApp
{
private:
    ftxui::App &app_;

    std::string path_; // UTF-8
    std::shared_ptr<TableBook> book_;
    // One view per sheet, created when the sheet is first shown.
    std::vector<std::shared_ptr<TuiTableView>> views_;
    // Positions to restore in sheets that have no view yet (after a revert).
    std::map<std::string, TuiTableView::ScrollPosition> restore_positions_;
    std::map<std::string, ViewSpec> restore_views_;
    // Columns whose statistics are started again, like restore_views_.
    std::map<std::string, size_t> restore_stats_;
    std::map<std::string, HiddenColumns> restore_hidden_;
    std::map<std::string, size_t> restore_frozen_;
    size_t sheet_ = 0;
    std::filesystem::file_time_type file_mtime_{};
    std::uintmax_t file_size_ = 0;

    bool first_row_as_header_ = false;
    // Set by #; unset: the file's default (on for VCF).
    std::optional<bool> skip_meta_lines_;
    // Set by E or --encoding; unset: detected.
    std::optional<TextEncoding> text_encoding_;
    bool auto_revert_ = true;
    bool show_sidebar_ = false;
    // What the sidebar shows: the cursor's cell, or column statistics (S).
    enum class SidebarMode
    {
        Cell,
        Stats,
    };
    SidebarMode sidebar_mode_ = SidebarMode::Cell;

    // Convert to Parquet (P) / DuckDB (D), or export the view (w): runs on
    // convert_thread_, which posts progress. One at a time.
    ExportFormat convert_format_ = ExportFormat::Parquet;
    enum class Overwrite
    {
        None,
        Convert,
        Export,
    };
    Overwrite confirm_overwrite_ = Overwrite::None; // waiting for y / n
    bool converting_ = false;
    std::string convert_label_; // "converting" / "exporting", in the status bar
    size_t convert_rows_ = 0;

    // Export prompt (w): the target, prefilled next to the file; the format
    // follows its extension.
    bool export_prompt_ = false;
    std::string export_input_;
    std::filesystem::path export_target_;
    // The sheet the prompt was opened for; a click on another tab must not
    // export that one instead.
    std::shared_ptr<TableModel> export_model_;

    std::string status_message_;
    bool status_is_error_ = false;

    // Search (/, n, N). Only the current sheet is searched; another sheet
    // searches again when shown.
    enum class SearchJump
    {
        None,
        Next,
        Previous,
    };
    bool search_prompt_ = false;
    std::string search_input_;
    bool search_regex_ = false; // toggled with Ctrl+R in the prompt
    SearchSpec search_spec_;
    SearchState search_; // the current sheet's, refreshed on model updates
    // n / N waiting for hits that the search has not reached yet.
    SearchJump pending_jump_ = SearchJump::None;
    bool pending_jump_inclusive_ = false;

    // SQL prompt (:), for files with SQL support. The last query is run again
    // when the file is reopened, since its TEMP view goes with the old book.
    bool sql_prompt_ = false;
    std::string sql_input_;
    // SQL run before, shared with the GUI and other windows (Up / Down).
    SqlHistory sql_history_;
    SqlHistoryCursor sql_history_cursor_;
    // Names for Tab completion, loaded when the prompt opens and after SQL ran.
    std::vector<SqlTable> sql_schema_;
    // Repeated Tab cycles through the candidates for one word.
    struct SqlCompletionCycle
    {
        std::string head; // the input before the word
        std::vector<std::string> candidates;
        size_t next = 0;
    };
    std::optional<SqlCompletionCycle> sql_completion_;
    bool sql_running_ = false;
    std::string last_query_;
    std::string sql_sheet_;
    bool sql_reselect_ = false; // the query's sheet was shown before a reopen

    // Reading a CSV / TSV paused at a memory checkpoint: asks whether to go on (y / n).
    std::shared_ptr<TableModel> memory_prompt_model_;

    // Histogram (B): shown instead of the sheet, of the model it was started
    // on; the selected bar can filter the sheet.
    std::shared_ptr<TableModel> histogram_model_;
    size_t histogram_selected_ = 0;
    size_t histogram_top_ = 0; // first bar shown when they do not all fit

    // Go To prompt (Ctrl+G): a row, cell or column.
    bool goto_prompt_ = false;
    std::string goto_input_;

    // Edit mode (I), kept when the same file is reopened. A cell is edited
    // in the status line, like the prompts.
    bool editing_ = false;
    bool edit_prompt_ = false;
    std::string edit_input_;
    std::shared_ptr<TableModel> edit_model_;
    size_t edit_row_ = 0; // view row
    size_t edit_col_ = 0;
    // Pasted in edit mode outside a prompt: goes into the selected cells.
    std::string paste_buffer_;
    bool saving_ = false;
    // The file changed while there were unsaved edits (no Auto Revert then).
    bool changed_on_disk_ = false;
    // Sheet names of a book closed to replace its file, for reopening it.
    std::vector<std::string> closed_sheet_names_;
    // Asked before unsaved edits would be lost: y saves (or, when the file is
    // read again anyway, discards), n discards, Esc cancels; then the action runs.
    std::function<void()> unsaved_action_;
    bool unsaved_reload_ = false;

    // Filter dialog (f): edits a copy of the current sheet's filter, applied
    // with Enter. Lines: 0 is "Match", then one per condition, then "+ Add".
    enum class FilterField
    {
        Column,
        Operator,
        Value,
    };
    bool show_filter_dialog_ = false;
    Filter filter_draft_;
    size_t filter_line_ = 0;
    FilterField filter_field_ = FilterField::Value;

    // Text dialog (help, errors)
    bool show_text_dialog_ = false;
    // Between the bracketed paste markers: pasted keys are text, not commands.
    bool pasting_ = false;
    std::string dialog_title_;
    std::vector<std::string> dialog_lines_;
    bool dialog_wrap_ = true;
    // dialog_lines_ wrapped to dialog_rows_width_ columns
    std::vector<std::string> dialog_rows_;
    int dialog_rows_width_ = -1;
    size_t dialog_scroll_ = 0;
    int dialog_page_ = 10;

    ftxui::Component table_holder_;
    ftxui::Component root_;
    std::vector<ftxui::Box> tab_boxes_;

    // Declared last: stopped first, before anything they post to goes away.
    std::jthread convert_thread_;
    std::jthread auto_revert_thread_;

    TuiTableView::Poster poster();
    TuiTableView *currentView() const;
    void showSheet(size_t index);
    void revert();
    void revertIfChanged();
    bool headerOptionAvailable() const;
    void setFirstRowAsHeader(bool enabled);
    bool skipMetaLinesAvailable() const;
    bool skipsMetaLines() const;
    void setSkipMetaLines(bool enabled);
    TextEncoding textEncoding() const;
    void convertTo(ExportFormat format);
    void startConversion();
    void openExportPrompt();
    bool handleExportPromptEvent(ftxui::Event event);
    ftxui::Element renderExportPrompt();
    void exportTo(const std::string &text);
    void startExport();
    void copySelection();
    void showHelp();
    void showTextDialog(std::string title, const std::string &text, bool wrap = true);
    void showError(const std::string &message);
    void setStatus(std::string message, bool error = false);
    void runSql(const std::string &sql, bool rerun);
    void sqlFinished(const std::shared_ptr<TableBook> &book, const std::string &sql, bool rerun,
                     const TableBook::SqlResult &result);
    void startSearch(std::string query);
    void syncSearch();
    void refreshSearch();
    void jumpToHit(bool forward, bool include_from = false);
    std::string searchStatusText() const;
    bool filterAvailable();
    void openFilterDialog();
    void addFilterDraftCondition();
    bool applyFilterDraft();
    void quickFilter(bool equal);
    void clearFilter();
    bool setFilter(std::optional<Filter> filter);
    std::string filterStatusText() const;

    bool editing() const;
    void setEditing(bool editing);
    bool requireEditMode();
    void editCell();
    bool handleEditPromptEvent(ftxui::Event event);
    ftxui::Element renderEditPrompt();
    void finishEditPrompt(bool commit, int rows, int cols);
    void clearSelectedCells();
    void undoEdit(bool redo);
    void pasteCells(const std::string &text);
    void save(std::function<void()> then = {});
    void saveFinished(std::shared_ptr<TableBook> book, const std::filesystem::path &target,
                      const TableBook::SaveResult &result, const std::function<void()> &then);
    // Runs `action` now, or after asking about unsaved edits.
    void checkUnsaved(std::function<void()> action, bool reload = false);
    bool handleUnsavedPromptEvent(ftxui::Event event);
    ftxui::Element renderUnsavedPrompt();

    bool handleEvent(ftxui::Event event);
    bool handlePasteEvent(const ftxui::Event &event, const ftxui::Component &target);
    bool typingText() const;
    bool refuseReopen();
    bool handleTextDialogEvent(ftxui::Event event);
    bool handleSearchPromptEvent(ftxui::Event event);
    bool handleSqlPromptEvent(ftxui::Event event);
    void completeSqlInput();
    void loadSqlSchema();
    bool handleFilterDialogEvent(ftxui::Event event);
    const std::vector<std::string> &dialogRows() const;
    ftxui::Element renderMain();
    ftxui::Element renderTabs();
    ftxui::Element renderSidebar(int width);
    ftxui::Element renderStatsSidebar(int width);
    void showColumnStats();
    void showHistogram(size_t column);
    void closeHistogram();
    bool handleHistogramEvent(ftxui::Event event);
    ftxui::Element renderHistogram();
    ftxui::Element renderStatusBar();
    ftxui::Element renderSearchPrompt();
    ftxui::Element renderSqlPrompt();
    ftxui::Element renderGoToPrompt();
    bool handleGoToPromptEvent(ftxui::Event event);
    void checkMemoryCheckpoint(const std::shared_ptr<TableModel> &model);
    bool handleMemoryPromptEvent(ftxui::Event event);
    ftxui::Element renderMemoryPrompt();
    void goTo(const std::string &text);
    ftxui::Element renderTextDialog();
    ftxui::Element renderFilterDialog();

public:
    explicit TuiApp(ftxui::App &app);
    ~TuiApp();
    TuiApp(const TuiApp &) = delete;
    TuiApp &operator=(const TuiApp &) = delete;

    // Opens a file (path in UTF-8). Reopening the current file keeps the
    // selected sheet and each sheet's position. On failure, the error is
    // stored in *error if given, else shown in a dialog.
    bool open(const std::string &path, std::string *error = nullptr);
    // Reopens the file in `encoding`; before open(), only records it.
    void setTextEncoding(TextEncoding encoding);

    ftxui::Component component() const { return root_; }
};

#endif // _TUI_APP_HXX_
