#ifndef _TABLE_HXX_
#define _TABLE_HXX_

// UI-facing table API.
//
//   TableBook  : one opened file. Owns the DataSource and one worker thread.
//   TableModel : one sheet. Every public method returns immediately: data that
//                is not cached yet is reported as CellState::Loading, requested
//                from the worker, and announced through a TableEvent once ready.
//
// Listeners are called on the worker thread; UIs must forward to their own
// main loop (Fl::awake, ftxui::ScreenInteractive::Post, ...).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "column_stats.hxx"
#include "histogram.hxx"
#include "scatter.hxx"
#include "table_types.hxx"
#include "table_source.hxx"

enum class CellState
{
    Ready,
    Loading,    // not cached yet; a request has been queued
    Empty,      // row exists but is shorter than this column (ragged)
    OutOfRange, // beyond the end of the table
    Error,      // the sheet failed to load
};

struct CellRef
{
    CellState state = CellState::Loading;
    RowBlockPtr block;                // keeps `value` alive while the CellRef is held
    const CellValue *value = nullptr; // non-null only when state == Ready
};

struct TableEvent
{
    enum class Kind
    {
        Opened,
        RowsLoaded, // source rows [first, last)
        RowCountChanged,
        ColumnsChanged,
        ViewChanged,
        SearchProgress,
        StatsProgress,
        HistogramProgress,
        ScatterProgress,
        MemoryCheckpoint, // reading paused, see TableModel::memoryCheckpoint()
        CellsChanged,     // edited, undone or redone (see TableModel::setCells())
        Error,
    };
    Kind kind;
    size_t first = 0;
    size_t last = 0;
    std::string message;
};

enum class ModelState
{
    Opening,
    Ready,
    Error,
};

struct ViewCapabilities
{
    bool sort = false;
    bool filter = false;
    bool search = true;
};

struct SearchState
{
    bool active = false;
    bool done = false;
    bool truncated = false; // stopped after too many hits
    std::string error;      // e.g. an invalid regular expression; the search is done
    size_t scanned_rows = 0;
    std::vector<CellPosition> hits; // in view rows, in scan order
    // Changes whenever the search starts over (new search, cancel, or a view
    // change that moves the hits), even if the hit count stays the same.
    uint64_t generation = 0;
};

struct ColumnStatsState
{
    bool active = false;
    bool done = false;
    size_t column = 0;
    size_t scanned_rows = 0; // view rows; `stats` may lag behind until done
    ColumnStats stats;       // published now and then while scanning; final once done
    // Changes whenever the statistics start over (new column, cancel, or a
    // view change), even if the column stays the same.
    uint64_t generation = 0;
};

struct HistogramState
{
    bool active = false;
    bool done = false;
    size_t column = 0;
    size_t target_bins = 0;
    // The first pass finds the range; the second counts the values into
    // bins, which `histogram` then has (counts grow until done).
    bool counting = false;
    size_t scanned_rows = 0; // view rows of the current pass
    Histogram histogram;
    // Changes whenever the histogram starts over (new column, cancel, or a
    // view change), even if the column stays the same.
    uint64_t generation = 0;
};

struct ScatterState
{
    bool active = false;
    bool done = false;
    size_t x_column = 0;
    size_t y_column = 0;
    ScatterOptions options;
    // The first pass finds the ranges; the second counts the rows into the
    // grid and samples the points, which `plot` then has (growing until done).
    bool counting = false;
    size_t scanned_rows = 0; // view rows of the current pass
    ScatterPlot plot;
    // Changes whenever the plot starts over (new columns, cancel, or a view
    // change).
    uint64_t generation = 0;
};

// SearchState::hits is in scan order: by view row, then by column.
bool searchHitBefore(const CellPosition &a, const CellPosition &b);
// Index of the hit at `position`, if there is one.
std::optional<size_t> searchHitIndex(const SearchState &state, CellPosition position);

// Unsubscribes on destruction. After reset()/destruction returns, the
// listener is guaranteed not to be running and never to be called again.
// When reset() is called by the listener itself, the current invocation is
// allowed to finish, but the listener will not be called again.
class Subscription
{
private:
    std::function<void()> cancel_;

public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Subscription(Subscription &&other) noexcept : cancel_(std::exchange(other.cancel_, nullptr)) {}
    Subscription &operator=(Subscription &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            cancel_ = std::exchange(other.cancel_, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    ~Subscription() { reset(); }

    void reset()
    {
        if (cancel_)
        {
            std::exchange(cancel_, nullptr)();
        }
    }
};

namespace detail
{
    class Scheduler;
    class ViewScan;
}

// A chunk of the rows of a view, in order; `keep_alive` keeps `rows` valid.
struct ViewRows
{
    std::vector<RowBlockPtr> keep_alive;
    std::vector<const TableRow *> rows;
};

class TableModel;

// Reads every row of a model's view, e.g. for an export (see
// TableModel::readView()). Destroying it stops the reading.
class ViewReader
{
private:
    std::shared_ptr<TableModel> model_;
    std::shared_ptr<detail::ViewScan> scan_;

public:
    ViewReader(std::shared_ptr<TableModel> model, std::shared_ptr<detail::ViewScan> scan);
    ~ViewReader();
    ViewReader(const ViewReader &) = delete;
    ViewReader &operator=(const ViewReader &) = delete;

    // Blocks until the next rows are read. Returns an empty chunk after the
    // last row, and nullopt once `stop` is requested. Throws
    // std::runtime_error if the sheet fails or the view changes meanwhile.
    std::optional<ViewRows> next(std::stop_token stop);
};

namespace re2
{
    class RE2;
}

class TableModel : public std::enable_shared_from_this<TableModel>
{
public:
    struct Options
    {
        size_t block_rows = 512;
        size_t cache_bytes = 256 * 1024 * 1024; // random-access sources only
        // Sequential sources keep every row read in memory, which a sort or
        // filter needs. For delimited text (TableBook clears both otherwise),
        // reading pauses when the rows read reach each of these sizes, in
        // bytes, and then each multiple of the interval above them, until
        // continueReading() or stopReading(). Empty and 0: never.
        std::vector<size_t> memory_checkpoints = {size_t{100} << 20, size_t{500} << 20, size_t{1} << 30};
        size_t memory_checkpoint_interval = size_t{2} << 30;
        size_t max_search_hits = 100000;
        ColumnStatsLimits stats_limits;
        // readView(): rows read at a time from a random-access source, past
        // the cache; twice as many are read ahead of the reader.
        size_t scan_read_rows = 8192;
    };

    using Listener = std::function<void(const TableEvent &)>;

    ~TableModel();

    const std::string &name() const { return name_; }

    // --- non-blocking data access (any thread, usually the UI thread) ---
    CellRef peek(size_t view_row, size_t column);
    // Tells the worker which view rows [first, last] are visible. The latest
    // call wins; nearby rows are prefetched.
    void requestRange(size_t first, size_t last);

    ModelState state() const;
    std::string errorMessage() const;
    // Message of a sort/filter that the source failed to run since the last
    // call; the model then went back to the unsorted, unfiltered table.
    std::optional<std::string> takeViewError();
    RowCount rowCount() const; // rows in the current view
    // Rows before filtering; rowCount() while no filter is applied. Not exact
    // if a pushed-down filter was applied before the rows were counted.
    RowCount unfilteredRowCount() const;
    // Row count for a scrollable widget: exact when known, otherwise the
    // loaded rows plus some slack so that scrolling down requests more.
    size_t displayRowCount() const;
    size_t columnCount() const;
    std::string columnName(size_t column) const; // header name or column letter
    bool hasHeader() const;
    std::vector<CellRange> mergedCells() const;

    Subscription subscribe(Listener listener);

    // --- sort / filter / search ---
    ViewCapabilities viewCapabilities() const;
    // Applied asynchronously; ViewChanged (or Error) is emitted when done.
    void setView(ViewSpec spec);
    ViewSpec view() const; // the applied view
    // The view being applied if setView() is still pending, else view(); a
    // base for the next change, so that quick changes build on each other.
    ViewSpec requestedView() const;

    // An invalid regular expression ends the search at once with
    // SearchState::error set.
    void startSearch(SearchSpec spec);
    void cancelSearch();
    SearchState searchState() const;

    // Statistics of one column over the rows of the view, computed in the
    // background like a search; StatsProgress is emitted as rows are scanned.
    // A view change starts them over.
    void startColumnStats(size_t column);
    void cancelColumnStats();
    ColumnStatsState columnStatsState() const;

    struct MemoryCheckpoint
    {
        size_t bytes = 0; // the rows read so far, approximately
        size_t rows = 0;
    };
    // Set while reading is paused at a memory checkpoint (see
    // Options::memory_checkpoint_bytes): rows not read yet stay Loading, and
    // sorts, filters, searches and statistics that need them wait.
    std::optional<MemoryCheckpoint> memoryCheckpoint() const;
    // The first checkpoint above `bytes` (Options::memory_checkpoints, then
    // multiples of the interval); 0 if there is none.
    static size_t nextMemoryCheckpoint(const Options &options, size_t bytes);
    // Reads on until the next checkpoint.
    void continueReading();
    // Ends the table at the rows read so far, as if the file ended there.
    void stopReading();
    // Whether stopReading() cut the table short.
    bool readingStopped() const;

    // A histogram of one column over the rows of the view, computed in the
    // background in two passes; HistogramProgress is emitted as rows are
    // counted. A view change starts it over.
    void startHistogram(size_t column, size_t target_bins = 30);
    void cancelHistogram();
    HistogramState histogramState() const;

    // Two columns plotted against each other over the rows of the view
    // (points and a 2D density), computed in the background in two passes
    // like a histogram; ScatterProgress is emitted as rows are counted. A
    // view change starts it over.
    void startScatter(size_t x_column, size_t y_column, ScatterOptions options = {});
    void cancelScatter();
    ScatterState scatterState() const;

    // Reads the rows of the view (the one being applied, if setView() is
    // pending) in order on the worker, a few blocks ahead of the reader. One
    // reader at a time: a new one makes the previous one fail.
    std::unique_ptr<ViewReader> readView();

    // --- editing (see TableBook::setEditing()) ---
    // How typed text becomes a value; None if the sheet cannot be edited.
    CellEditKind editKind() const;
    // Whether the book is in edit mode and this sheet can be edited.
    bool editMode() const;
    // Whether the cell can be changed now: the book is in edit mode, the
    // row is loaded, and the cell is not locked (an Excel formula, say, or
    // part of a merged range other than its top left cell).
    bool cellEditable(size_t view_row, size_t column) const;
    struct CellText
    {
        size_t row = 0; // view row
        size_t column = 0;
        std::string text; // converted by editedCellValue()
    };
    // Changes the cells at once, as one step for undo(); cells that are not
    // editable are left alone. CellsChanged is emitted, and searches,
    // statistics and histograms start over, but the view is not applied
    // again: an edited row stays where it is until the next setView().
    // Returns how many cells changed.
    size_t setCells(const std::vector<CellText> &cells);
    // Take back the last step, or do it again, in edit mode; `cell` gets
    // the first cell it changed if that is in the view. False if there was
    // nothing to undo or redo.
    bool undo(CellPosition *cell = nullptr);
    bool redo(CellPosition *cell = nullptr);
    bool canUndo() const;
    bool canRedo() const;
    // Whether some cell differs from the file (edits that are undone again
    // do not count).
    bool modified() const;
    bool cellModified(size_t view_row, size_t column) const;

private:
    friend class TableBook;
    friend class ViewReader;
    friend class detail::Scheduler;

    struct ListenerEntry
    {
        explicit ListenerEntry(Listener value) : listener(std::move(value)) {}

        Listener listener;
        std::mutex mutex;
        std::condition_variable cv;
        bool active = true;
        size_t in_flight = 0;
        std::thread::id running_thread;
    };

    struct CachedBlock
    {
        RowBlockPtr block;
        uint64_t last_used = 0;
    };

    TableModel(std::string name, Options options, std::weak_ptr<detail::Scheduler> scheduler);

    // Worker thread only. Performs one unit of work; false if idle.
    bool step(DataSource &data_source, std::stop_token stop);
    uint64_t priority() const { return priority_.load(); }
    // Called after the worker has stopped.
    void detachSource();
    // Worker thread only: the sheet's rows were replaced (a new query). The
    // model drops its source and is never stepped again.
    void retire();

    // Editing, for TableBook.
    struct CellChange
    {
        size_t row = 0; // source row
        size_t column = 0;
        CellValue before;
        CellValue after;
    };
    void setEditing(bool editing);
    void discardEdits();
    SheetEdits sheetEdits() const;
    bool cellEditableLocked(size_t source_row, size_t column) const;
    std::optional<size_t> sourceRowOfLocked(size_t view_row) const;
    std::optional<size_t> viewRowOfLocked(size_t source_row) const;
    // Sets each change's `after` (or `before`, backwards) in a copy of its block.
    void applyChangesLocked(const std::vector<CellChange> &changes, bool forward);
    void cellsChangedLocked();
    void restartSearchLocked();
    bool stepEdits(bool forward, CellPosition *cell);

    // helpers for step(); `lock` is held on entry and exit, released around I/O
    bool stepOpen(DataSource &data_source, std::vector<TableEvent> &events);
    std::optional<size_t> nextBlockToLoad() const;
    bool loadBlock(size_t block_index, std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    bool stepApplyView(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    // Scans of the whole view (search, column statistics) go a block at a time.
    struct ScanChunk
    {
        size_t end = 0;    // the view row after the last one collected
        bool last = false; // the end of the view was reached
        std::vector<RowBlockPtr> keep_alive;
        std::vector<std::pair<size_t, const TableRow *>> rows; // view row, row
    };
    std::optional<size_t> scanBlockToLoadLocked(size_t start) const;
    ScanChunk scanChunkLocked(size_t start) const;
    bool stepSearch(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    bool stepStats(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    bool stepStatsPushdown(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    void restartStatsLocked();
    bool stepHistogram(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    void finishHistogramPassLocked(std::vector<TableEvent> &events);
    void restartHistogramLocked();
    bool stepScatter(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    void finishScatterPassLocked(std::vector<TableEvent> &events);
    void restartScatterLocked();
    bool stepCount(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    bool stepScan(std::unique_lock<std::mutex> &lock, std::stop_token stop, std::vector<TableEvent> &events);
    bool scanWantsRowsLocked() const;
    void failScanLocked(const std::string &message);
    void evictLocked(std::optional<size_t> keep_block = std::nullopt);
    const TableRow *sourceRowLocked(size_t source_row, RowBlockPtr *keep_alive = nullptr) const;
    bool allRowsCachedLocked() const;
    void emit(const std::vector<TableEvent> &events);
    void wakeWorker();

    const std::string name_;
    const Options options_;
    const std::weak_ptr<detail::Scheduler> scheduler_;
    std::atomic<uint64_t> priority_{0};

    // worker thread only
    std::unique_ptr<SheetSource> source_;
    bool count_attempted_ = false;

    mutable std::mutex mutex_;
    ModelState state_ = ModelState::Opening;
    std::string error_message_;
    std::optional<std::string> view_error_;
    SourceCapabilities capabilities_;
    std::map<size_t, CachedBlock> blocks_; // block index -> block
    size_t cache_bytes_ = 0;
    uint64_t use_clock_ = 0;
    size_t sequential_rows_ = 0; // sequential sources: rows read so far
    bool eof_ = false;
    RowCount source_row_count_;
    RowCount unfiltered_row_count_; // pushdown sources while a filter is applied
    std::vector<ColumnInfo> columns_;
    size_t column_count_ = 0;
    std::vector<CellRange> merged_cells_;
    std::optional<std::pair<size_t, size_t>> viewport_; // view rows, inclusive
    std::optional<size_t> adhoc_block_;                 // requested by peek() outside the viewport
    ViewSpec view_;
    std::optional<ViewSpec> pending_view_;
    std::shared_ptr<const std::vector<size_t>> view_rows_; // in-memory view: view row -> source row
    std::optional<SearchSpec> search_spec_;
    std::shared_ptr<const re2::RE2> search_regex_; // compiled search_spec_ (regex searches only)
    SearchState search_;
    size_t search_next_row_ = 0;
    uint64_t search_generation_ = 0; // bumped on start/cancel so in-flight scans are discarded
    // Column statistics: only the worker touches the accumulator's contents.
    std::shared_ptr<ColumnStatsAccumulator> stats_accumulator_;
    ColumnStatsState stats_;
    size_t stats_next_row_ = 0;
    bool stats_pushdown_tried_ = false; // SheetSource::columnValueCounts(), before scanning
    uint64_t stats_generation_ = 0;
    std::chrono::steady_clock::time_point stats_published_;
    // Histogram: the worker's progress (only it touches the accumulators'
    // contents), reset once done.
    struct HistogramJob
    {
        size_t next_row = 0;
        bool pushdown_tried = false; // SheetSource::columnValueCounts() for this pass
        std::shared_ptr<HistogramRange> range;
        std::shared_ptr<HistogramCounter> counter;
    };
    std::optional<HistogramJob> histogram_job_;
    HistogramState histogram_;
    uint64_t histogram_generation_ = 0;
    std::chrono::steady_clock::time_point histogram_published_;
    // Scatter plot, like the histogram.
    struct ScatterJob
    {
        size_t next_row = 0;
        bool pushdown_tried = false; // SheetSource::columnValueCounts() for the first pass
        std::shared_ptr<ScatterRange> range;
        std::shared_ptr<ScatterCounter> counter;
    };
    std::optional<ScatterJob> scatter_job_;
    ScatterState scatter_;
    uint64_t scatter_generation_ = 0;
    std::chrono::steady_clock::time_point scatter_published_;
    std::shared_ptr<detail::ViewScan> scan_; // readView()
    bool detached_ = false;                  // the book is gone: nothing reads rows any more
    size_t next_memory_checkpoint_ = 0;      // cache_bytes_ at which reading pauses; 0: never
    bool memory_paused_ = false;
    bool stop_reading_pending_ = false; // stopReading(), carried out by the worker
    bool reading_stopped_ = false;
    bool editing_ = false;
    std::vector<std::vector<CellChange>> edits_; // steps; [0, edit_pos_) are done
    size_t edit_pos_ = 0;
    std::map<size_t, TableRow> original_rows_; // source row -> as read, for every row edited

    std::mutex listeners_mutex_;
    std::vector<std::shared_ptr<ListenerEntry>> listeners_;
};

class TableBook
{
public:
    // Opens a file with the backend chosen by the registry. Throws std::runtime_error.
    static std::shared_ptr<TableBook> open(const std::filesystem::path &path,
                                           const OpenOptions &options = {},
                                           const BackendRegistry &registry = BackendRegistry::builtin(),
                                           TableModel::Options model_options = {});

    explicit TableBook(std::unique_ptr<DataSource> data_source, TableModel::Options model_options = {});
    ~TableBook();
    TableBook(const TableBook &) = delete;
    TableBook &operator=(const TableBook &) = delete;

    // A query run with executeSql() may add a sheet.
    std::vector<std::string> sheetNames() const;
    // See DataSource::usesHeaderOption().
    bool usesHeaderOption() const { return uses_header_option_; }
    // See DataSource::isTextFormat().
    bool isTextFormat() const { return is_text_format_; }
    // See DataSource::skipsMetaLines().
    bool skipsMetaLines() const { return skips_meta_lines_; }
    // See DataSource::textEncoding().
    std::optional<TextEncoding> textEncoding() const { return text_encoding_; }
    // Models are created lazily and opened on the worker thread. nullptr if no such sheet.
    std::shared_ptr<TableModel> sheet(const std::string &sheet_name);

    struct SqlResult
    {
        bool ok = false;
        std::string sheet;   // the query's sheet (see DataSource::executeSql())
        std::string message; // the error if !ok
    };
    // See DataSource::supportsSql().
    bool supportsSql() const { return supports_sql_; }
    // Runs DataSource::executeSql() on the worker, after the step in progress.
    // When a query replaces the rows of an existing sheet, that sheet's model
    // is retired (it reports an error from then on); sheet() returns a new one.
    // `done` is called on the worker thread.
    void executeSql(std::string sql, std::function<void(const SqlResult &)> done);
    // Runs DataSource::sqlSchema() on the worker, like executeSql(); `done`
    // gets nothing if it fails.
    void loadSqlSchema(std::function<void(std::vector<SqlTable>)> done);

    // --- editing ---
    // See DataSource::supportsEditing().
    bool supportsEditing() const { return supports_editing_; }
    // Edit mode, for every sheet: cells can be changed only while it is on
    // (it is off at first, and stays off unless supportsEditing()). Turning
    // it off keeps the edits.
    void setEditing(bool editing);
    bool editing() const;
    // Whether some sheet has edits that are not saved.
    bool modified() const;
    // Undoes every edit of every sheet.
    void discardEdits();

    struct SaveResult
    {
        bool ok = false;
        std::filesystem::path written; // the new file, for commitSave()
        std::string message;           // the error if !ok
    };
    // Writes the file with the edits (DataSource::saveEdits()) on the worker,
    // after the step in progress, to a new file next to `target`; then close
    // the book and move it there with commitSave(): on Windows, a file that
    // is being read cannot be replaced. Fails if the opened file has changed
    // since open(). `done` is called on the worker thread.
    void saveEdits(std::filesystem::path target, std::function<void(const SaveResult &)> done);

private:
    std::unique_ptr<DataSource> data_source_;
    std::filesystem::path path_; // open() only: the file and its stamp then
    std::filesystem::file_time_type file_mtime_{};
    std::uintmax_t file_size_ = 0;
    bool has_file_stamp_ = false;
    bool supports_editing_ = false;
    bool editing_ = false; // guarded by models_mutex_
    std::vector<std::string> sheet_names_; // guarded by models_mutex_
    bool uses_header_option_ = false;
    bool supports_sql_ = false;
    bool is_text_format_ = false;
    bool skips_meta_lines_ = false;
    std::optional<TextEncoding> text_encoding_;
    TableModel::Options model_options_;
    std::shared_ptr<detail::Scheduler> scheduler_;
    mutable std::mutex models_mutex_;
    std::map<std::string, std::shared_ptr<TableModel>> models_;
    std::jthread worker_; // declared last: started after, and stopped before, everything above
};

// Moves a file written by TableBook::saveEdits() to its target, replacing
// it. Throws std::filesystem::filesystem_error; the file is then left where
// it is.
void commitSave(const std::filesystem::path &written, const std::filesystem::path &target);

#endif // _TABLE_HXX_
