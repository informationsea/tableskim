#ifndef _TABLE_EXPORT_HXX_
#define _TABLE_EXPORT_HXX_

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <memory>
#include <string>

#include "cell_format.hxx"
#include "table_source.hxx"

class TableModel;

enum class ExportFormat
{
    Parquet,
    DuckDb, // one table, named after the source file (see exportTableName())
};

// "Parquet", "DuckDB"
const char *exportFormatName(ExportFormat format);
// ".parquet", ".duckdb"
const char *exportFormatExtension(ExportFormat format);
// From the extension (.parquet; .duckdb, .ddb), case-insensitively; nullopt otherwise.
std::optional<ExportFormat> exportFormatFromPath(const std::filesystem::path &path);

// Converts a delimited text file (DataSource::isTextFormat()) to `format`.
// The file is read with the same backend and options as when it is viewed,
// so skipped "##" lines, the header choice and ragged rows match what is
// shown; sorts and filters are not applied. Columns get the narrowest type
// every value fits (BIGINT, DOUBLE, BOOLEAN, DATE, TIMESTAMP, else VARCHAR);
// empty cells become NULL in typed columns. Without a header, columns are
// named A, B, C, ... as in the viewer.
//
// The target is replaced only once it has been written completely. Returns
// the number of rows written, or nullopt if stopped. `progress` is called
// with the rows read so far, on the calling thread. Throws std::runtime_error.
std::optional<size_t> exportTextFile(const std::filesystem::path &source, const std::filesystem::path &target,
                                     ExportFormat format, const OpenOptions &options, std::stop_token stop = {},
                                     const std::function<void(size_t rows)> &progress = {});

// "dir/data.csv.gz" -> "dir/data.parquet" / "dir/data.duckdb"
std::filesystem::path defaultExportPath(const std::filesystem::path &source, ExportFormat format);

// Name of the table in a DuckDB export: "dir/data.csv.gz" -> "data".
std::string exportTableName(const std::filesystem::path &source);

// Formats of exportView().
enum class ViewExportFormat
{
    Csv,
    Tsv,
    Excel, // .xlsx with one sheet
    Parquet,
};

inline constexpr ViewExportFormat VIEW_EXPORT_FORMATS[] = {ViewExportFormat::Csv, ViewExportFormat::Tsv,
                                                           ViewExportFormat::Excel, ViewExportFormat::Parquet};

// "CSV", "TSV", "Excel", "Parquet"
const char *viewExportFormatName(ViewExportFormat format);
// ".csv", ".tsv", ".xlsx", ".parquet"
const char *viewExportFormatExtension(ViewExportFormat format);
// From the extension (.csv; .tsv, .tab; .xlsx; .parquet), case-insensitively; nullopt otherwise.
std::optional<ViewExportFormat> viewExportFormatFromPath(const std::filesystem::path &path);

struct ViewExportOptions
{
    HiddenColumns hidden;         // columns left out
    std::filesystem::path source; // the file being viewed, which must not be the target
};

// Writes the rows of the model's view, sorted and filtered as shown, to
// `target`. The rows are read through the model (TableModel::readView()),
// so this works for every sheet, including SQL results.
//
// CSV/TSV hold each cell as formatCell() shows it, quoted only where needed
// (RFC 4180), in UTF-8, with a header row if the sheet has one. Excel and
// Parquet keep typed values (numbers, booleans, dates, times); a column of
// text gets the type every value fits, as in exportTextFile(). Parquet
// columns are named after the header, or A, B, C, ... without one.
//
// The target is replaced only once it has been written completely. Returns
// the number of rows written, or nullopt if stopped. `progress` is called
// with the rows read so far, on the calling thread. Throws
// std::runtime_error, e.g. if the view changes meanwhile.
std::optional<size_t> exportView(const std::shared_ptr<TableModel> &model, const std::filesystem::path &target,
                                 ViewExportFormat format, const ViewExportOptions &options = {},
                                 std::stop_token stop = {}, const std::function<void(size_t rows)> &progress = {});

// "dir/data.csv.gz" -> "dir/data-export.xlsx"; with a sheet name (for files
// with several sheets) "dir/book-Sheet1-export.csv".
std::filesystem::path defaultViewExportPath(const std::filesystem::path &source, const std::string &sheet,
                                            ViewExportFormat format);

#endif // _TABLE_EXPORT_HXX_
