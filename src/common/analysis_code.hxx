#ifndef _ANALYSIS_CODE_HXX_
#define _ANALYSIS_CODE_HXX_

// Code in R or Python that repeats what TableSkim shows: reads the file,
// applies the view's filter (and sort), and then draws the histogram or the
// scatter plot, or computes the Group By. It is meant as a starting point
// that is easy to read, not as an exact copy: plots keep the libraries'
// default colours, and filters and aggregates follow the libraries' types
// (TableSkim's own rules are close but not the same; see analysis_code.cxx).

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "column_type.hxx"
#include "group_by.hxx"
#include "table_types.hxx"
#include "text_encoding.hxx"

class TableBook;
class TableModel;

enum class CodeLanguage
{
    R,      // tidyverse (dplyr, ggplot2, readr)
    Polars, // Python: polars and plotnine
    Pandas, // Python: pandas and matplotlib
};
inline constexpr CodeLanguage CODE_LANGUAGES[] = {CodeLanguage::R, CodeLanguage::Polars, CodeLanguage::Pandas};
// "R (tidyverse)", "Python (polars + plotnine)", "Python (pandas + matplotlib)"
const char *codeLanguageName(CodeLanguage language);
// "r", "polars", "pandas": for command lines and the web app's protocol.
const char *codeLanguageId(CodeLanguage language);
std::optional<CodeLanguage> parseCodeLanguage(std::string_view id);
// ".R" or ".py"
const char *codeLanguageExtension(CodeLanguage language);

// How the code reads the file.
enum class CodeFormat
{
    Text, // CSV, TSV, ... (compressed too)
    Excel,
    Ods, // OpenDocument spreadsheets
    Parquet,
    Json,
    NdJson,
    Sqlite,
    DuckDb,
};

struct CodeSource
{
    std::string path; // UTF-8, as written into the code
    CodeFormat format = CodeFormat::Text;
    // The sheet or table of a file that has several (Excel, databases);
    // empty for the others.
    std::string table;
    // Text: the delimiter, if known (else the code guesses it, or says to
    // check it), whether lines starting with "##" are skipped, and the
    // encoding (UTF-8 is not mentioned).
    std::optional<char> delimiter;
    bool skip_meta_lines = false;
    TextEncoding encoding = TextEncoding::Utf8;
    // Text and Excel: whether the first row holds the column names; if not,
    // the code names the columns as TableSkim does (`columns`).
    bool header = true;
    std::vector<std::string> columns;
    // What each column holds (ColumnKind::Unknown past the end): filters on
    // numbers, dates and booleans compare values of that type.
    std::vector<ColumnKind> kinds;

    std::string columnName(size_t column) const;
    ColumnKind kind(size_t column) const;
};

// The format a file is read in by the code, from TableBook::isTextFormat(),
// its extension and its first bytes (SQLite and DuckDB databases).
CodeFormat codeFormatOf(const std::filesystem::path &path, bool text_format);

// The source of a sheet of an opened file. `table` is left empty for files
// with a single sheet. Not for sheets without a file behind them (SQL
// results, Group By). `delimiter`: OpenOptions::delimiter.
CodeSource codeSourceOf(const std::filesystem::path &path, const TableBook &book, const TableModel &model,
                        std::optional<char> delimiter = std::nullopt);

struct HistogramCodeSpec
{
    size_t column = 0;
    std::optional<size_t> group_column; // stacked by this column's values
    size_t bins = 30;
    bool operator==(const HistogramCodeSpec &) const = default;
};

struct ScatterCodeSpec
{
    size_t x_column = 0; // a text column makes box plots
    size_t y_column = 0;
    std::optional<size_t> color_column;
    bool operator==(const ScatterCodeSpec &) const = default;
};

// What the code does after the filter: nothing (the view itself), a Group
// By, a histogram or a scatter plot.
using CodeAnalysis = std::variant<std::monostate, GroupBySpec, HistogramCodeSpec, ScatterCodeSpec>;

struct AnalysisCodeSpec
{
    CodeSource source;
    // Its filter is applied before the analysis; its sort only without one
    // (a plot or a Group By does not depend on the order of the rows).
    ViewSpec view;
    // Without an analysis: the columns left out, as an export of the view
    // leaves them out.
    std::vector<size_t> hidden;
    CodeAnalysis analysis;
};

// The code, as UTF-8 text with "\n" line ends.
std::string analysisCode(const AnalysisCodeSpec &spec, CodeLanguage language);
// A file name for the code: "<file stem>-<analysis>" with the language's
// extension, "-polars" / "-pandas" before ".py" ("data-histogram.R",
// "data-group-by-pandas.py").
std::string analysisCodeFileName(const AnalysisCodeSpec &spec, CodeLanguage language);
// The language a file name chosen for code is in (".R", or "polars" or
// "pandas" in the name of a ".py" file), if it tells.
std::optional<CodeLanguage> codeLanguageFromFileName(const std::filesystem::path &path);
// The file to save code of `language` as, from the one chosen in a file
// chooser that suggested analysisCodeFileName(): a suggested name (of any
// language, as the chooser may leave it or only change its extension when
// another language is picked) becomes the language's own, and another
// name gets the language's extension unless it has it.
std::filesystem::path analysisCodeTarget(const AnalysisCodeSpec &spec, const std::filesystem::path &chosen,
                                         CodeLanguage language);

#endif // _ANALYSIS_CODE_HXX_
