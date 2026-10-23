#ifndef _TABLE_SOURCE_HXX_
#define _TABLE_SOURCE_HXX_

// Backend interface. Implement SheetSource + DataSource for a file format and
// register a BackendFactory; TableBook/TableModel take care of threading,
// caching and notifications.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "column_stats.hxx"
#include "sql_text.hxx"
#include "table_types.hxx"
#include "text_encoding.hxx"

struct SourceCapabilities
{
    // false: rows can only be read from the start, in order (CSV, Excel).
    bool random_access = false;
    // true: sort/filter can be pushed down to the backend (DuckDB, SQLite, Parquet).
    bool query_pushdown = false;
    // Whether cells can be edited (and saved with DataSource::saveEdits()),
    // and how typed text becomes a value. Only for sequential sources: an
    // edit is kept in the model's cache, which they never evict.
    CellEditKind edits = CellEditKind::None;
};

// The edited rows of one sheet, for DataSource::saveEdits().
struct SheetEdits
{
    struct Row
    {
        size_t row = 0;    // source row, as read() numbers it
        TableRow original; // as read
        TableRow edited;
    };
    std::string sheet;
    std::vector<Row> rows; // by row
};

// One sheet / table. All methods are called from the owning TableBook's
// worker thread only, so implementations need no locking.
class SheetSource
{
public:
    virtual ~SheetSource() = default;

    virtual SourceCapabilities capabilities() const = 0;

    // Column names known so far. Ragged sources may return fewer names than
    // the widest row; the model fills the gap with letters.
    virtual std::vector<ColumnInfo> columns() = 0;

    // What is cheaply known. Sequential sources usually return Unknown.
    virtual RowCount rowCount() = 0;

    // An expensive exact count (e.g. COUNT(*)), run in the background when
    // rowCount() is not exact. Return nullopt if unsupported.
    virtual std::optional<size_t> countRowsSlow(std::stop_token) { return std::nullopt; }

    // Reads up to max_rows rows starting at first_row. A block with fewer rows
    // than requested means end of data.
    // For sequential sources first_row is always the row after the previous read.
    virtual RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token stop) = 0;

    virtual std::vector<CellRange> mergedCells() { return {}; }

    // Called only when capabilities().query_pushdown is true. Subsequent
    // read()/rowCount() calls must reflect the new view.
    virtual void applyView(const ViewSpec &) {}

    // Column statistics and histograms without reading every row (pushdown
    // sources): passes each distinct value of the column in the applied view
    // to `add` with its count, most frequent first, so that the values
    // counted before ColumnStatsAccumulator's distinct limit are the ones
    // that matter. Values must be converted as read() does. Returns the
    // number of distinct values, or nullopt if unsupported or stopped; the
    // model then scans the rows.
    using ValueCountSink = std::function<void(const CellValue &value, size_t count)>;
    virtual std::optional<size_t> columnValueCounts(size_t /*column*/, const ValueCountSink & /*add*/,
                                                    std::stop_token)
    {
        return std::nullopt;
    }
};

struct SqlOutcome
{
    std::string sheet;   // non-empty: the query's rows are shown in this sheet
    std::string message; // otherwise, what the statements did
};

// One file / database. Constructed on the caller's thread (so that open
// errors can be reported synchronously); afterwards only touched by the worker.
class DataSource
{
public:
    virtual ~DataSource() = default;
    virtual std::vector<std::string> sheetNames() = 0;
    virtual std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) = 0;

    // Whether OpenOptions::header changes anything. Formats that store column
    // names separately (Parquet, databases) ignore it.
    virtual bool usesHeaderOption() const { return false; }
    // Delimited text (CSV, TSV, ...): OpenOptions::skip_meta_lines and encoding apply,
    // and the file can be converted with convertToParquet().
    virtual bool isTextFormat() const { return false; }
    // Whether lines starting with "##" are dropped, after applying the default.
    virtual bool skipsMetaLines() const { return false; }
    // Text formats: the encoding the file is read in, after detection.
    virtual std::optional<TextEncoding> textEncoding() const { return std::nullopt; }

    // Whether executeSql() is supported (DuckDB, SQLite, Parquet and JSON files).
    virtual bool supportsSql() const { return false; }
    // Runs SQL against the file, which is open read-only: only queries and
    // temporary object operations are allowed; SQL cannot write external files.
    // A single query (SELECT, WITH, VALUES, ...) is
    // kept as a TEMP view instead, shown as the sheet named in the outcome;
    // the next query replaces it. sheetNames() then includes that sheet.
    // Throws std::runtime_error.
    virtual SqlOutcome executeSql(const std::string &sql);
    // The tables and views SQL can name, with their columns, for completion:
    // the file's and temporary ones, but not TableSkim's own. Throws
    // std::runtime_error.
    virtual std::vector<SqlTable> sqlSchema() { return {}; }

    // Whether saveEdits() is supported: its sheets have
    // SourceCapabilities::edits (CSV, Excel).
    virtual bool supportsEditing() const { return false; }
    // Writes the file with the edits applied to `target`, a new file in the
    // same format, leaving everything that was not edited as it is. Each
    // row's `original` must match the file; the file is not touched.
    // Throws std::runtime_error.
    virtual void saveEdits(const std::vector<SheetEdits> &edits, const std::filesystem::path &target, std::stop_token stop);
};

struct OpenOptions
{
    enum class Header
    {
        Auto,
        Yes,
        No,
    };
    Header header = Header::Auto;
    std::optional<char> delimiter; // text formats: override delimiter detection
    // Text formats: drop lines starting with "##", such as VCF meta-information.
    // Unset: only for VCF files (.vcf, .vcf.gz, ...).
    std::optional<bool> skip_meta_lines;
    // Text formats: the character encoding. Unset: detected from the contents.
    std::optional<TextEncoding> encoding;
};

struct BackendFactory
{
    std::string name;
    // Returns a score (0 = cannot open). `head` holds the first bytes of the file.
    std::function<int(const std::filesystem::path &, std::span<const std::byte> head)> probe;
    std::function<std::unique_ptr<DataSource>(const std::filesystem::path &, const OpenOptions &)> open;
};

class BackendRegistry
{
private:
    std::vector<BackendFactory> factories_;

public:
    void add(BackendFactory factory);

    // Picks the factory with the highest probe score. Throws std::runtime_error.
    std::unique_ptr<DataSource> open(const std::filesystem::path &path, const OpenOptions &options = {}) const;

    // Registry with every built-in backend.
    static const BackendRegistry &builtin();
};

#endif // _TABLE_SOURCE_HXX_
