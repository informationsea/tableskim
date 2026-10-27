#include "table_summary.hxx"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "cell_format.hxx"
#include "file_util.hxx"
#include "text_encoding.hxx"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    // Markdown cells are cut short; JSON keeps every value as it is.
    constexpr size_t markdown_cell_bytes = 80;

    struct ColumnScan
    {
        ColumnStatsAccumulator stats;
        ColumnTypeCounter types;
    };

    // The rows of a sheet read from the start: the first ones kept, the
    // column types and (if wanted) statistics counted, the rest dropped.
    struct SheetScan
    {
        const SummaryOptions &options;
        ColumnStatsLimits limits;
        bool count_stats = false;
        std::vector<ColumnScan> columns;
        std::vector<TableRow> head;
        size_t rows = 0;
        bool eof = false;

        void widen(size_t width)
        {
            while (columns.size() < width)
            {
                columns.push_back({ColumnStatsAccumulator(limits), {}});
                // The rows before had no cell here (ragged rows): empty.
                if (count_stats)
                    columns.back().stats.add(CellValue{}, rows);
            }
        }

        void add(const TableRow &row)
        {
            widen(row.size());
            if (head.size() < options.head_rows)
                head.push_back(row);
            const bool sample = rows < options.type_sample_rows;
            for (size_t c = 0; c < columns.size(); ++c)
            {
                const CellValue &value = c < row.size() ? row[c] : CellValue{};
                if (sample)
                    columns[c].types.add(value);
                if (count_stats)
                    columns[c].stats.add(value);
            }
            ++rows;
        }

        void read(SheetSource &sheet, size_t limit, std::stop_token stop)
        {
            while (rows < limit && !eof)
            {
                if (stop.stop_requested())
                    throw std::runtime_error("Stopped");
                const size_t wanted = std::min(options.block_rows, limit - rows);
                const auto block = sheet.read(rows, wanted, stop);
                const size_t got = block ? block->rows.size() : 0;
                if (block)
                {
                    for (const auto &row : block->rows)
                        add(row);
                }
                if (got < wanted)
                    eof = true;
            }
        }
    };

    std::string jsonString(std::string_view text)
    {
        // Text from the files is UTF-8, but not always valid.
        std::string repaired;
        if (auto fixed = repairUtf8(text))
        {
            repaired = std::move(*fixed);
            text = repaired;
        }
        std::string out = "\"";
        for (const char ch : text)
        {
            const auto c = static_cast<unsigned char>(ch);
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                {
                    out += ch;
                }
            }
        }
        out += '"';
        return out;
    }

    std::string jsonNumber(double value)
    {
        // JSON has no NaN or infinities.
        if (!std::isfinite(value))
            return jsonString(std::isnan(value) ? "NaN" : value > 0 ? "Infinity" : "-Infinity");
        return shortestDoubleText(value);
    }

    // Numbers and booleans as such; dates and date-times as ISO text.
    std::string jsonValue(const CellValue &value)
    {
        struct Visitor
        {
            std::string operator()(std::monostate) const { return "null"; }
            std::string operator()(const std::string &v) const { return jsonString(v); }
            std::string operator()(int64_t v) const { return std::to_string(v); }
            std::string operator()(double v) const { return jsonNumber(v); }
            std::string operator()(bool v) const { return v ? "true" : "false"; }
            std::string operator()(Date v) const { return jsonString(formatCell(CellValue{v})); }
            std::string operator()(DateTime v) const { return jsonString(formatCell(CellValue{v})); }
        };
        return std::visit(Visitor{}, value);
    }

    // '|' escaped, for a table cell.
    std::string markdownCell(std::string_view text)
    {
        std::string out;
        for (const char c : text)
        {
            if (c == '|')
                out += "\\|";
            else
                out += c;
        }
        return out;
    }

    // A one-line table cell, cut short.
    std::string markdownValue(const CellValue &value)
    {
        return markdownCell(cellDisplayText(value, markdown_cell_bytes));
    }

    std::string markdownName(const std::string &name)
    {
        return markdownValue(CellValue{name});
    }

    std::string markdownRow(const std::vector<std::string> &cells)
    {
        std::string out = "|";
        for (const auto &cell : cells)
            out += " " + cell + " |";
        return out + "\n";
    }

    std::string markdownSeparator(size_t columns)
    {
        std::string out = "|";
        for (size_t i = 0; i < columns; ++i)
            out += " --- |";
        return out + "\n";
    }

    std::string rowsText(const RowCount &rows)
    {
        switch (rows.kind)
        {
        case RowCount::Kind::Exact:
            return std::to_string(rows.value) + (rows.value == 1 ? " row" : " rows");
        case RowCount::Kind::AtLeast:
            return "at least " + std::to_string(rows.value) + " rows";
        case RowCount::Kind::Unknown:
            break;
        }
        return "an unknown number of rows";
    }

    std::string topValuesText(const ColumnStats &stats)
    {
        std::string out;
        for (const auto &top : stats.top)
        {
            if (!out.empty())
                out += ", ";
            out += markdownValue(top.value) + " (" + std::to_string(top.count) + ")";
        }
        return out;
    }

    // Means and standard deviations to 8 significant digits, which is
    // plenty to read (JSON keeps every digit).
    std::string doubleCell(const std::optional<double> &value)
    {
        if (!value)
            return {};
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.8g", *value);
        return buffer;
    }
} // namespace

const char *columnKindName(ColumnKind kind)
{
    switch (kind)
    {
    case ColumnKind::Unknown:
        return "unknown";
    case ColumnKind::Number:
        return "number";
    case ColumnKind::Date:
        return "date";
    case ColumnKind::DateTime:
        return "datetime";
    case ColumnKind::Boolean:
        return "boolean";
    case ColumnKind::Text:
        return "text";
    case ColumnKind::Mixed:
        return "mixed";
    }
    return "unknown";
}

SheetSummary summarizeSheet(SheetSource &sheet, const std::string &name, const SummaryOptions &options,
                            std::stop_token stop)
{
    SheetSummary summary;
    summary.name = name;
    const auto capabilities = sheet.capabilities();
    ColumnStatsLimits limits;
    limits.top_values = options.top_values;

    // Pushdown sources count each column in the database (as the UIs'
    // statistics do), far faster than reading every row.
    std::vector<ColumnStats> counted;
    if (options.stats && !options.max_rows && capabilities.query_pushdown)
    {
        const size_t width = sheet.columns().size();
        for (size_t c = 0; c < width; ++c)
        {
            ColumnStatsAccumulator accumulator(limits);
            if (!sheet.columnValueCounts(
                    c, [&](const CellValue &value, size_t count)
                    { accumulator.add(value, count); },
                    stop))
            {
                counted.clear();
                break;
            }
            counted.push_back(accumulator.result());
        }
    }

    SheetScan scan{options, limits};
    scan.count_stats = options.stats && counted.empty();
    size_t limit = std::max(options.head_rows, options.type_sample_rows);
    if (scan.count_stats)
        limit = options.max_rows.value_or(std::numeric_limits<size_t>::max());
    scan.read(sheet, limit, stop);

    if (scan.eof)
    {
        summary.rows = {scan.rows, RowCount::Kind::Exact};
    }
    else
    {
        summary.rows = sheet.rowCount();
        if (!summary.rows.isExact() && capabilities.random_access)
        {
            if (const auto count = sheet.countRowsSlow(stop))
                summary.rows = {*count, RowCount::Kind::Exact};
        }
        if (!summary.rows.isExact() && summary.rows.value <= scan.rows)
            summary.rows = {scan.rows, RowCount::Kind::AtLeast};
    }

    // Read after the rows: a header may only be known then.
    const auto infos = sheet.columns();
    const size_t width = std::max({infos.size(), scan.columns.size(), counted.size()});
    scan.widen(width);
    for (size_t c = 0; c < width; ++c)
    {
        ColumnSummary column;
        column.name = c < infos.size() && !infos[c].name.empty() ? infos[c].name : columnLetter(c);
        column.type = scan.columns[c].types.type();
        if (!counted.empty())
            column.stats = c < counted.size() ? counted[c] : ColumnStats{};
        else if (scan.count_stats)
            column.stats = scan.columns[c].stats.result();
        summary.columns.push_back(std::move(column));
    }
    if (!counted.empty())
    {
        summary.stats_rows = counted.front().rows;
        summary.stats_complete = true;
    }
    else if (scan.count_stats)
    {
        summary.stats_rows = scan.rows;
        summary.stats_complete = scan.eof;
    }
    summary.head = std::move(scan.head);
    return summary;
}

FileSummary summarizeFile(const std::filesystem::path &path, const std::string &file_name,
                          const OpenOptions &open_options, const SummaryOptions &options,
                          const BackendRegistry &registry, std::stop_token stop)
{
    FileSummary summary;
    summary.file = file_name;
    std::unique_ptr<DataSource> source;
    try
    {
        source = registry.open(path, open_options);
    }
    catch (const std::exception &e)
    {
        summary.error = e.what();
        return summary;
    }
    summary.encoding = source->textEncoding();

    auto names = source->sheetNames();
    if (!options.sheets.empty())
    {
        for (const auto &wanted : options.sheets)
        {
            if (std::find(names.begin(), names.end(), wanted) == names.end())
            {
                summary.error = "No such sheet: " + wanted;
                return summary;
            }
        }
        names = options.sheets;
    }
    for (const auto &name : names)
    {
        try
        {
            auto sheet = source->openSheet(name);
            if (!sheet)
                throw std::runtime_error("No such sheet: " + name);
            summary.sheets.push_back(summarizeSheet(*sheet, name, options, stop));
        }
        catch (const std::exception &e)
        {
            SheetSummary failed;
            failed.name = name;
            failed.error = e.what();
            summary.sheets.push_back(std::move(failed));
        }
    }
    return summary;
}

std::string summaryMarkdown(const FileSummary &summary, const SummaryOptions &options)
{
    std::string out = "# " + summary.file + "\n\n";
    if (!summary.error.empty())
        return out + "Error: " + summary.error + "\n";
    if (summary.encoding)
        out += std::string("Encoding: ") + textEncodingName(*summary.encoding) + "\n\n";
    if (summary.sheets.size() > 1)
    {
        out += std::to_string(summary.sheets.size()) + " sheets:";
        for (size_t i = 0; i < summary.sheets.size(); ++i)
            out += (i ? ", " : " ") + summary.sheets[i].name;
        out += "\n\n";
    }

    for (const auto &sheet : summary.sheets)
    {
        out += "## " + markdownName(sheet.name) + "\n\n";
        if (!sheet.error.empty())
        {
            out += "Error: " + sheet.error + "\n\n";
            continue;
        }
        out += rowsText(sheet.rows) + ", " + std::to_string(sheet.columns.size()) +
               (sheet.columns.size() == 1 ? " column" : " columns") + ".";
        if (options.stats && !sheet.stats_complete)
            out += " Statistics over the first " + std::to_string(sheet.stats_rows) + " rows.";
        out += "\n\n";

        if (!sheet.columns.empty())
        {
            std::vector<std::string> header = {"#", "Column", "Type"};
            if (options.stats)
            {
                for (const char *label : {"Empty", "Distinct", "Min", "Max", "Mean", "Std dev", "Top values"})
                    header.push_back(label);
            }
            out += markdownRow(header) + markdownSeparator(header.size());
            for (size_t c = 0; c < sheet.columns.size(); ++c)
            {
                const auto &column = sheet.columns[c];
                std::vector<std::string> cells = {std::to_string(c + 1), markdownName(column.name),
                                                  columnTypeText(column.type)};
                if (column.stats)
                {
                    const auto &stats = *column.stats;
                    cells.push_back(std::to_string(stats.empty) + " (" + percentText(stats.empty, stats.rows) + ")");
                    cells.push_back((stats.distinct_exact ? "" : "≥") + std::to_string(stats.distinct));
                    cells.push_back(stats.min ? markdownValue(*stats.min) : std::string());
                    cells.push_back(stats.max ? markdownValue(*stats.max) : std::string());
                    cells.push_back(doubleCell(stats.mean));
                    cells.push_back(doubleCell(stats.stddev));
                    cells.push_back(topValuesText(stats));
                }
                cells.resize(header.size());
                out += markdownRow(cells);
            }
            out += "\n";
        }

        if (!sheet.head.empty())
        {
            out += sheet.head.size() == 1 ? std::string("First row:\n\n")
                                          : "First " + std::to_string(sheet.head.size()) + " rows:\n\n";
            std::vector<std::string> header;
            for (const auto &column : sheet.columns)
                header.push_back(markdownName(column.name));
            out += markdownRow(header) + markdownSeparator(header.size());
            for (const auto &row : sheet.head)
            {
                std::vector<std::string> cells(header.size());
                for (size_t c = 0; c < row.size() && c < cells.size(); ++c)
                    cells[c] = markdownValue(row[c]);
                out += markdownRow(cells);
            }
            out += "\n";
        }
    }
    return out;
}

std::string summaryJson(const FileSummary &summary, const SummaryOptions &options)
{
    std::string out = "{\n  \"file\": " + jsonString(summary.file);
    if (!summary.error.empty())
        return out + ",\n  \"error\": " + jsonString(summary.error) + "\n}";
    if (summary.encoding)
        out += ",\n  \"encoding\": " + jsonString(textEncodingName(*summary.encoding));
    out += ",\n  \"sheets\": [";
    for (size_t s = 0; s < summary.sheets.size(); ++s)
    {
        const auto &sheet = summary.sheets[s];
        out += s ? ",\n" : "\n";
        out += "    {\n      \"name\": " + jsonString(sheet.name);
        if (!sheet.error.empty())
        {
            out += ",\n      \"error\": " + jsonString(sheet.error) + "\n    }";
            continue;
        }
        out += ",\n      \"rows\": " +
               (sheet.rows.kind == RowCount::Kind::Unknown ? std::string("null") : std::to_string(sheet.rows.value));
        out += ",\n      \"rows_exact\": " + std::string(sheet.rows.isExact() ? "true" : "false");
        if (options.stats)
        {
            out += ",\n      \"stats_rows\": " + std::to_string(sheet.stats_rows);
            out += ",\n      \"stats_complete\": " + std::string(sheet.stats_complete ? "true" : "false");
        }
        out += ",\n      \"columns\": [";
        for (size_t c = 0; c < sheet.columns.size(); ++c)
        {
            const auto &column = sheet.columns[c];
            out += c ? ",\n" : "\n";
            out += "        {\"name\": " + jsonString(column.name) + ", \"type\": \"" +
                   columnKindName(column.type.kind) + "\"";
            if (column.type.kind == ColumnKind::Number)
                out += std::string(", \"integers\": ") + (column.type.integers ? "true" : "false");
            if (column.stats)
            {
                const auto &stats = *column.stats;
                out += ",\n         \"stats\": {\"rows\": " + std::to_string(stats.rows) +
                       ", \"empty\": " + std::to_string(stats.empty) +
                       ", \"distinct\": " + std::to_string(stats.distinct) +
                       ", \"distinct_exact\": " + (stats.distinct_exact ? "true" : "false") +
                       ", \"numbers\": " + std::to_string(stats.numbers);
                if (stats.min)
                    out += ", \"min\": " + jsonValue(*stats.min);
                if (stats.max)
                    out += ", \"max\": " + jsonValue(*stats.max);
                if (stats.sum)
                    out += ", \"sum\": " + jsonNumber(*stats.sum);
                if (stats.mean)
                    out += ", \"mean\": " + jsonNumber(*stats.mean);
                if (stats.stddev)
                    out += ", \"stddev\": " + jsonNumber(*stats.stddev);
                out += ", \"top\": [";
                for (size_t i = 0; i < stats.top.size(); ++i)
                {
                    out += i ? ", " : "";
                    out += "{\"value\": " + jsonValue(stats.top[i].value) +
                           ", \"count\": " + std::to_string(stats.top[i].count) + "}";
                }
                out += "]}";
            }
            out += "}";
        }
        out += sheet.columns.empty() ? "]" : "\n      ]";
        out += ",\n      \"head\": [";
        for (size_t r = 0; r < sheet.head.size(); ++r)
        {
            out += r ? ",\n        [" : "\n        [";
            const auto &row = sheet.head[r];
            for (size_t c = 0; c < row.size(); ++c)
                out += (c ? ", " : "") + jsonValue(row[c]);
            out += "]";
        }
        out += sheet.head.empty() ? "]" : "\n      ]";
        out += "\n    }";
    }
    out += summary.sheets.empty() ? "]" : "\n  ]";
    return out + "\n}";
}

namespace
{
#ifdef _WIN32
    // TableSkim.exe is a GUI application: unless its output is redirected,
    // it has no standard output, so write to the console it was started from.
    void attachParentConsole()
    {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if ((handle == nullptr || handle == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
        {
            FILE *stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
            freopen_s(&stream, "CONOUT$", "w", stderr);
        }
        SetConsoleOutputCP(CP_UTF8);
    }
#endif

    void writeOut(const std::string &text)
    {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
} // namespace

int runSummary(const CommandLineOptions &options)
{
#ifdef _WIN32
    attachParentConsole();
#endif
    std::string error;
    // Logging would mix with the output; stderr only with --verbose.
    if (!configureLogging(options, options.verbose, &error))
    {
        std::fprintf(stderr, "Cannot open log file %s: %s\n", options.log_file.c_str(), error.c_str());
        return 1;
    }

    SummaryOptions summary_options;
    summary_options.stats = options.summary;
    if (options.head)
        summary_options.head_rows = *options.head;
    summary_options.sheets = options.sheets;
    summary_options.max_rows = options.scan_rows;
    OpenOptions open_options;
    open_options.encoding = options.encoding;
    if (options.header)
        open_options.header = *options.header ? OpenOptions::Header::Yes : OpenOptions::Header::No;

    const bool json = options.summary_format == SummaryFormat::Json;
    const bool array = json && options.files.size() > 1;
    if (array)
        writeOut("[\n");
    int exit_code = 0;
    for (size_t i = 0; i < options.files.size(); ++i)
    {
        const auto &file = options.files[i];
        const auto summary = summarizeFile(utf8Path(file), file, open_options, summary_options);
        if (!summary.error.empty())
        {
            std::fprintf(stderr, "Failed to open %s: %s\n", file.c_str(), summary.error.c_str());
            exit_code = 1;
        }
        for (const auto &sheet : summary.sheets)
        {
            if (!sheet.error.empty())
            {
                std::fprintf(stderr, "Failed to read %s, sheet %s: %s\n", file.c_str(), sheet.name.c_str(),
                             sheet.error.c_str());
                exit_code = 1;
            }
        }
        if (json)
            writeOut(summaryJson(summary, summary_options) + (array && i + 1 < options.files.size() ? ",\n" : "\n"));
        else
            writeOut((i ? "\n" : "") + summaryMarkdown(summary, summary_options));
        std::fflush(stdout);
    }
    if (array)
        writeOut("]\n");
    std::fflush(stdout);
    return exit_code;
}
