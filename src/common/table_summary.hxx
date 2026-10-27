#ifndef _TABLE_SUMMARY_HXX_
#define _TABLE_SUMMARY_HXX_

// A summary of a file for the command line (--summary, --head): its
// sheets, their columns with types and statistics, and the first rows,
// written to standard output as Markdown or JSON.
//
// Unlike the UIs, the summary reads each sheet's source directly on the
// calling thread, streaming the rows: nothing but the first rows is kept,
// so that a file of any size can be summarized.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "cli_options.hxx"
#include "column_stats.hxx"
#include "column_type.hxx"
#include "table_source.hxx"
#include "table_types.hxx"

struct SummaryOptions
{
    bool stats = true;     // column statistics (over every row unless max_rows)
    size_t head_rows = 10; // the first rows shown
    // The sheets summarized; empty: every sheet.
    std::vector<std::string> sheets;
    // Statistics over the first rows only. Without it, pushdown sources
    // (DuckDB, Parquet) count each column in the database instead.
    std::optional<size_t> max_rows;
    size_t top_values = 5; // most frequent values per column
    // Column types are judged from the first rows, as the UIs' header marks are.
    size_t type_sample_rows = 1000;
    size_t block_rows = 4096; // rows read at a time
};

struct ColumnSummary
{
    std::string name; // header name or column letter
    ColumnType type;
    std::optional<ColumnStats> stats; // SummaryOptions::stats only
};

struct SheetSummary
{
    std::string name;
    RowCount rows;
    std::vector<ColumnSummary> columns;
    size_t stats_rows = 0;       // rows the statistics are over
    bool stats_complete = false; // over every row of the sheet
    std::vector<TableRow> head;
    std::string error; // the sheet could not be read; the rest may be partial
};

struct FileSummary
{
    std::string file; // as given, UTF-8
    std::optional<TextEncoding> encoding;
    std::vector<SheetSummary> sheets;
    std::string error; // the file could not be opened, or a sheet named is missing
};

FileSummary summarizeFile(const std::filesystem::path &path, const std::string &file_name,
                          const OpenOptions &open_options, const SummaryOptions &options,
                          const BackendRegistry &registry = BackendRegistry::builtin(), std::stop_token stop = {});
SheetSummary summarizeSheet(SheetSource &sheet, const std::string &name, const SummaryOptions &options,
                            std::stop_token stop = {});

std::string summaryMarkdown(const FileSummary &summary, const SummaryOptions &options);
// One JSON object per file (see docs/en for the fields).
std::string summaryJson(const FileSummary &summary, const SummaryOptions &options);
// "number", "date", "datetime", "boolean", "text", "mixed" or "unknown".
const char *columnKindName(ColumnKind kind);

// --summary / --head: prints the summaries of options.files to standard
// output (a JSON array for several files) and returns the exit code: 1 if
// a file or sheet failed, the error going to standard error too.
int runSummary(const CommandLineOptions &options);

#endif // _TABLE_SUMMARY_HXX_
