#include "table_export.hxx"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <duckdb.hpp>

#include "decompress.hxx"
#include "file_util.hxx"
#include "sql_text.hxx"
#include "table.hxx"
#include "table_duckdb.hxx"
#include "text_encoding.hxx"
#include "xlsx_writer.hxx"

namespace
{
    // Rows read and appended at a time.
    constexpr size_t BLOCK_ROWS = 10000;

    // Staging columns are c0, c1, ...; the real names may be anything.
    std::string stagingColumn(size_t index)
    {
        return "c" + std::to_string(index);
    }

    // Parquet and DuckDB need unique, non-empty column names.
    std::vector<std::string> uniqueNames(const std::vector<std::string> &names)
    {
        std::vector<std::string> result;
        std::set<std::string> used;
        for (size_t i = 0; i < names.size(); ++i)
        {
            const std::string base = names[i].empty() ? columnLetter(i) : names[i];
            std::string name = base;
            for (int n = 2; used.contains(name); ++n)
                name = base + "_" + std::to_string(n);
            used.insert(name);
            result.push_back(std::move(name));
        }
        return result;
    }

    // The narrowest type in which every non-empty value of each column fits.
    // The patterns keep what DuckDB's casts would also accept (spaces, "nan",
    // "1_000", leading zeros as in "007") as text.
    std::vector<std::string> inferTypes(duckdb::Connection &conn, const std::vector<size_t> &columns)
    {
        struct Candidate
        {
            const char *type;
            const char *pattern;
        };
        static const Candidate candidates[] = {
            {"BIGINT", "[+-]?(0|[1-9][0-9]*)"},
            {"DOUBLE", "[+-]?(0|[1-9][0-9]*)?(\\.[0-9]+)?([eE][+-]?[0-9]+)?"},
            {"BOOLEAN", "(?i)true|false"},
            {"DATE", "[0-9]{4}-[0-9]{2}-[0-9]{2}"},
            {"TIMESTAMP", "[0-9]{4}-[0-9]{2}-[0-9]{2}[ T][0-9]{2}:[0-9]{2}(:[0-9]{2}(\\.[0-9]+)?)?"},
        };
        std::vector<std::string> types(columns.size(), "VARCHAR");
        if (columns.empty())
            return types;
        std::string sql = "SELECT ";
        for (size_t i = 0; i < columns.size(); ++i)
        {
            const std::string v = stagingColumn(columns[i]);
            if (i > 0)
                sql += ", ";
            // An all-empty column stays text.
            sql += "count(NULLIF(" + v + ", '')) > 0";
            for (const auto &candidate : candidates)
            {
                sql += ", bool_and(" + v + " IS NULL OR " + v + " = '' OR (regexp_full_match(" + v + ", " +
                       quoteSqlString(candidate.pattern) + ") AND TRY_CAST(" + v + " AS " + candidate.type +
                       ") IS NOT NULL";
                // Text must round-trip through DOUBLE without losing digits:
                // integers even when another row makes BIGINT unsuitable,
                // decimals such as "1.10" only if they print back the same.
                if (std::string_view(candidate.type) == "DOUBLE")
                    sql += " AND coalesce(CASE WHEN regexp_full_match(" + v + ", " +
                           quoteSqlString(candidates[0].pattern) + ") THEN TRY_CAST(TRY_CAST(" + v +
                           " AS DOUBLE) AS BIGINT) = TRY_CAST(" + v + " AS BIGINT) ELSE TRY_CAST(TRY_CAST(" + v +
                           " AS DOUBLE) AS VARCHAR) = " + v + " END, FALSE)";
                sql += "))";
            }
        }
        sql += " FROM staging";
        auto result = conn.Query(sql);
        if (result->HasError())
            throw std::runtime_error(result->GetError());
        const size_t stride = 1 + std::size(candidates);
        if (result->RowCount() == 0)
            return types;
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (!result->GetValue(i * stride, 0).GetValue<bool>())
                continue;
            for (size_t c = 0; c < std::size(candidates); ++c)
            {
                const duckdb::Value fits = result->GetValue(i * stride + 1 + c, 0);
                if (!fits.IsNull() && fits.GetValue<bool>())
                {
                    types[i] = candidates[c].type;
                    break;
                }
            }
        }
        return types;
    }

    // Reserve a directory beside the target atomically. Concurrent exports
    // must never remove, publish or clean up another export's partial file.
    class TempFileGuard
    {
    private:
        std::filesystem::path directory_;

    public:
        explicit TempFileGuard(const std::filesystem::path &target)
        {
            std::random_device random;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const uint64_t tag = (static_cast<uint64_t>(random()) << 32) ^ random();
                const auto directory = target.parent_path() / (".tableskim-export-" + std::to_string(tag));
                std::error_code error;
                if (std::filesystem::create_directory(directory, error))
                {
                    directory_ = directory;
                    return;
                }
                if (error && error != std::errc::file_exists)
                    throw std::filesystem::filesystem_error("Cannot create an export directory", directory, error);
            }
            throw std::runtime_error("Cannot reserve a temporary export directory");
        }
        ~TempFileGuard()
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
        TempFileGuard(const TempFileGuard &) = delete;
        TempFileGuard &operator=(const TempFileGuard &) = delete;
        std::filesystem::path path() const { return directory_ / "data"; }
    };

    std::filesystem::path withSuffix(std::filesystem::path path, const char *suffix)
    {
        path += suffix;
        return path;
    }

    // Kinds of values seen in a staged column.
    enum ValueKind : uint8_t
    {
        KIND_TEXT = 1,
        KIND_INTEGER = 2,
        KIND_REAL = 4,
        KIND_BOOLEAN = 8,
        KIND_DATE = 16,
        KIND_TIMESTAMP = 32,
    };

    uint8_t valueKind(const CellValue &value)
    {
        struct Visitor
        {
            uint8_t operator()(std::monostate) const { return 0; }
            uint8_t operator()(const std::string &) const { return KIND_TEXT; }
            uint8_t operator()(int64_t) const { return KIND_INTEGER; }
            uint8_t operator()(double) const { return KIND_REAL; }
            uint8_t operator()(bool) const { return KIND_BOOLEAN; }
            uint8_t operator()(Date) const { return KIND_DATE; }
            uint8_t operator()(DateTime) const { return KIND_TIMESTAMP; }
        };
        return std::visit(Visitor{}, value);
    }

    // A private in-memory DuckDB in which rows are staged as text (c0, c1,
    // ...), since the column types are known only once every row has been
    // seen. It spills to a temporary directory, removed afterwards.
    class Staging
    {
    private:
        struct Cleanup
        {
            std::filesystem::path directory;
            ~Cleanup()
            {
                std::error_code ignored;
                std::filesystem::remove_all(directory, ignored);
            }
        };

        // Declared before the database, which must be closed first.
        Cleanup cleanup_;
        std::unique_ptr<duckdb::DuckDB> db_;
        std::unique_ptr<duckdb::Connection> conn_;
        std::stop_token stop_;
        std::unique_ptr<std::stop_callback<std::function<void()>>> interrupt_;
        std::unique_ptr<duckdb::Appender> appender_;
        size_t width_ = 0;
        std::vector<uint8_t> kinds_; // ValueKind bits seen per column
        std::vector<bool> integers_fit_double_;

    public:
        Staging(size_t width, std::stop_token stop) : cleanup_{duckDbTempDirectory()}, stop_(stop)
        {
            duckdb::DBConfig config;
            config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
            config.SetOptionByName("temp_directory", duckdb::Value(utf8String(cleanup_.directory)));
            db_ = std::make_unique<duckdb::DuckDB>(nullptr, &config);
            conn_ = std::make_unique<duckdb::Connection>(*db_);
            // Cancels a running query. DuckDB clears the interrupt when the
            // next query starts: check for a stop before a long one.
            interrupt_ = std::make_unique<std::stop_callback<std::function<void()>>>(
                stop_, [this]
                { conn_->Interrupt(); });
            width_ = std::max<size_t>(width, 1);
            kinds_.assign(width_, 0);
            integers_fit_double_.assign(width_, true);
            std::string sql = "CREATE TABLE staging (";
            for (size_t i = 0; i < width_; ++i)
                sql += (i > 0 ? ", " : "") + stagingColumn(i) + " VARCHAR";
            run(sql + ")");
            appender_ = std::make_unique<duckdb::Appender>(*conn_, "staging");
        }

        ~Staging()
        {
            appender_.reset();
            interrupt_.reset();
        }

        duckdb::Connection &connection() { return *conn_; }
        size_t width() const { return width_; }

        // Errors after a stop are those of the interrupted query.
        void run(const std::string &sql)
        {
            auto result = conn_->Query(sql);
            if (result->HasError() && !stop_.stop_requested())
                throw std::runtime_error(result->GetError());
        }

        // Adds staging columns up to `width` (ragged rows).
        void widen(size_t width)
        {
            if (width <= width_)
                return;
            if (appender_)
                appender_->Close();
            for (size_t i = width_; i < width; ++i)
                run("ALTER TABLE staging ADD COLUMN " + stagingColumn(i) + " VARCHAR");
            width_ = width;
            kinds_.resize(width_, 0);
            integers_fit_double_.resize(width_, true);
            appender_ = std::make_unique<duckdb::Appender>(*conn_, "staging");
        }

        void append(const TableRow &row)
        {
            widen(row.size());
            appender_->BeginRow();
            for (size_t i = 0; i < width_; ++i)
            {
                if (i >= row.size() || std::holds_alternative<std::monostate>(row[i]))
                {
                    appender_->Append(nullptr);
                    continue;
                }
                kinds_[i] |= valueKind(row[i]);
                if (const auto *integer = std::get_if<int64_t>(&row[i]))
                    integers_fit_double_[i] = integers_fit_double_[i] &&
                        compareCells(row[i], CellValue{static_cast<double>(*integer)}) == 0;
                if (const auto *text = std::get_if<std::string>(&row[i]))
                {
                    // DuckDB refuses invalid UTF-8, e.g. a Latin-1 byte in a
                    // file detected as UTF-8 from its first bytes.
                    const auto repaired = repairUtf8(*text);
                    const std::string &valid = repaired ? *repaired : *text;
                    appender_->Append(valid.data(), static_cast<uint32_t>(valid.size()));
                }
                else
                {
                    const std::string formatted = formatCell(row[i]);
                    appender_->Append(formatted.data(), static_cast<uint32_t>(formatted.size()));
                }
            }
            appender_->EndRow();
        }

        void close()
        {
            if (appender_)
                appender_->Close();
            appender_.reset();
        }

        // Typed values keep their type when a column holds only one kind
        // (exact integers and reals: DOUBLE; dates and times: TIMESTAMP); a column
        // with text gets the narrowest type its text fits (inferTypes()).
        std::vector<std::string> types()
        {
            std::vector<size_t> textual;
            for (size_t i = 0; i < width_; ++i)
            {
                if (kinds_[i] == 0 || (kinds_[i] & KIND_TEXT))
                    textual.push_back(i);
            }
            std::vector<std::string> types(width_, "VARCHAR");
            if (stop_.stop_requested())
                return types;
            std::vector<std::string> inferred;
            try
            {
                inferred = inferTypes(*conn_, textual);
            }
            catch (const std::runtime_error &)
            {
                // The interrupted query: the caller sees the stop and writes nothing.
                if (stop_.stop_requested())
                    return types;
                throw;
            }
            for (size_t i = 0; i < textual.size(); ++i)
                types[textual[i]] = inferred[i];
            for (size_t i = 0; i < width_; ++i)
            {
                const uint8_t kinds = kinds_[i];
                if (kinds == 0 || (kinds & KIND_TEXT))
                    continue;
                if ((kinds & ~KIND_INTEGER) == 0)
                    types[i] = "BIGINT";
                else if ((kinds & ~(KIND_INTEGER | KIND_REAL)) == 0)
                    types[i] = integers_fit_double_[i] ? "DOUBLE" : "VARCHAR";
                else if (kinds == KIND_BOOLEAN)
                    types[i] = "BOOLEAN";
                else if (kinds == KIND_DATE)
                    types[i] = "DATE";
                else if ((kinds & ~(KIND_DATE | KIND_TIMESTAMP)) == 0)
                    types[i] = "TIMESTAMP";
            }
            return types;
        }

        // The staged `columns`, cast to `types` and named `names` (one per column).
        static std::string select(const std::vector<size_t> &columns, const std::vector<std::string> &names,
                                  const std::vector<std::string> &types)
        {
            std::string sql = "SELECT ";
            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (i > 0)
                    sql += ", ";
                const std::string v = stagingColumn(columns[i]);
                const std::string &type = types[columns[i]];
                if (type == "VARCHAR")
                    sql += v;
                else
                    sql += "CAST(NULLIF(" + v + ", '') AS " + type + ")";
                sql += " AS " + quoteSqlIdentifier(names[i]);
            }
            if (columns.empty())
                sql += "NULL AS A";
            return sql + " FROM staging";
        }
    };

    std::vector<size_t> visibleColumns(size_t first, size_t last, const HiddenColumns &hidden)
    {
        std::vector<size_t> columns;
        for (size_t c = first; c < last; ++c)
        {
            if (!hidden.contains(c))
                columns.push_back(c);
        }
        return columns;
    }

    // RFC 4180: quoted only if it holds the delimiter, a quote or a line break.
    void appendField(std::string &line, const std::string &text, char delimiter)
    {
        if (text.find_first_of(std::string{delimiter, '"', '\r', '\n'}) == std::string::npos)
        {
            line += text;
            return;
        }
        line += '"';
        for (const char c : text)
        {
            line += c;
            if (c == '"')
                line += '"';
        }
        line += '"';
    }

    std::optional<size_t> writeDelimited(TableModel &model, ViewReader &reader, const std::filesystem::path &path,
                                         char delimiter, const HiddenColumns &hidden, std::stop_token stop,
                                         const std::function<void(size_t rows)> &progress)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("Cannot create " + utf8String(path));
        std::string text;
        auto flush = [&]
        {
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out)
                throw std::runtime_error("Failed to write " + utf8String(path));
            text.clear();
        };
        bool started = false;
        size_t width = 0;
        std::vector<size_t> columns;
        size_t rows = 0;
        while (true)
        {
            std::optional<ViewRows> chunk = reader.next(stop);
            if (!chunk)
                return std::nullopt;
            if (!started)
            {
                // The sheet is open once the first rows arrive.
                started = true;
                width = model.columnCount();
                columns = visibleColumns(0, width, hidden);
                if (model.hasHeader())
                {
                    for (size_t i = 0; i < columns.size(); ++i)
                    {
                        if (i > 0)
                            text += delimiter;
                        appendField(text, model.columnName(columns[i]), delimiter);
                    }
                    text += '\n';
                }
            }
            if (chunk->rows.empty())
                break;
            for (const TableRow *row : chunk->rows)
            {
                if (row->size() > width)
                {
                    // Columns beyond those known so far (ragged rows).
                    const auto more = visibleColumns(width, row->size(), hidden);
                    columns.insert(columns.end(), more.begin(), more.end());
                    width = row->size();
                }
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    if (i > 0)
                        text += delimiter;
                    std::string field = columns[i] < row->size() ? formatCell((*row)[columns[i]]) : std::string();
                    // A lone empty field must not become a blank line, which readers skip.
                    if (columns.size() == 1 && field.empty())
                        text += "\"\"";
                    else
                        appendField(text, field, delimiter);
                }
                text += '\n';
            }
            flush();
            rows += chunk->rows.size();
            if (progress)
                progress(rows);
        }
        flush();
        out.close();
        if (!out)
            throw std::runtime_error("Failed to write " + utf8String(path));
        return rows;
    }

    CellValue resultValue(duckdb::Vector &vector, const std::string &type, size_t row)
    {
        if (!duckdb::FlatVector::Validity(vector).RowIsValid(row))
            return std::monostate{};
        if (type == "BIGINT")
            return duckdb::FlatVector::GetData<int64_t>(vector)[row];
        if (type == "DOUBLE")
            return duckdb::FlatVector::GetData<double>(vector)[row];
        if (type == "BOOLEAN")
            return duckdb::FlatVector::GetData<bool>(vector)[row];
        if (type == "DATE")
            return Date{duckdb::FlatVector::GetData<duckdb::date_t>(vector)[row].days};
        if (type == "TIMESTAMP")
            return DateTime{duckdb::FlatVector::GetData<duckdb::timestamp_t>(vector)[row].value};
        const duckdb::string_t text = duckdb::FlatVector::GetData<duckdb::string_t>(vector)[row];
        return std::string(text.GetData(), text.GetSize());
    }

    // Excel and Parquet: staged first, as the types depend on every row.
    std::optional<size_t> writeTyped(TableModel &model, ViewReader &reader, const std::filesystem::path &path,
                                     ViewExportFormat format, const HiddenColumns &hidden, std::stop_token stop,
                                     const std::function<void(size_t rows)> &progress)
    {
        std::optional<Staging> staging;
        size_t rows = 0;
        bool header = false;
        while (true)
        {
            std::optional<ViewRows> chunk = reader.next(stop);
            if (!chunk)
                return std::nullopt;
            if (!staging)
            {
                // The sheet is open once the first rows arrive.
                staging.emplace(model.columnCount(), stop);
                header = model.hasHeader();
            }
            if (chunk->rows.empty())
                break;
            if (format == ViewExportFormat::Excel && rows + chunk->rows.size() + (header ? 1 : 0) > XlsxWriter::MAX_ROWS)
                throw std::runtime_error("An Excel sheet holds at most 1,048,576 rows");
            for (const TableRow *row : chunk->rows)
                staging->append(*row);
            rows += chunk->rows.size();
            if (progress)
                progress(rows);
        }
        staging->close();
        staging->widen(model.columnCount());

        const std::vector<size_t> columns = visibleColumns(0, staging->width(), hidden);
        if (format == ViewExportFormat::Excel && columns.size() > XlsxWriter::MAX_COLUMNS)
            throw std::runtime_error("An Excel sheet holds at most 16,384 columns");
        std::vector<std::string> header_names;
        for (const size_t c : columns)
            header_names.push_back(model.columnName(c));
        const std::vector<std::string> names = uniqueNames(header_names);
        const std::vector<std::string> types = staging->types();
        const std::string select = Staging::select(columns, names, types);
        if (stop.stop_requested())
            return std::nullopt;

        if (format == ViewExportFormat::Parquet)
        {
            staging->run("COPY (" + select + ") TO " + quoteSqlString(utf8String(path)) +
                         " (FORMAT parquet, COMPRESSION zstd)");
            return stop.stop_requested() ? std::nullopt : std::optional<size_t>(rows);
        }

        XlsxWriter writer(path, model.name(), header);
        std::vector<CellValue> cells;
        if (header)
        {
            cells.assign(header_names.begin(), header_names.end());
            writer.addRow(cells);
        }
        auto result = staging->connection().SendQuery(select);
        if (result->HasError())
        {
            if (stop.stop_requested())
                return std::nullopt;
            throw std::runtime_error(result->GetError());
        }
        while (!stop.stop_requested())
        {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0)
                break;
            chunk->Flatten();
            for (size_t r = 0; r < chunk->size(); ++r)
            {
                cells.clear();
                for (size_t i = 0; i < columns.size(); ++i)
                    cells.push_back(resultValue(chunk->data[i], types[columns[i]], r));
                writer.addRow(cells);
            }
        }
        if (stop.stop_requested())
            return std::nullopt;
        if (result->HasError())
            throw std::runtime_error(result->GetError());
        writer.finish();
        return rows;
    }

} // namespace

const char *exportFormatName(ExportFormat format)
{
    return format == ExportFormat::Parquet ? "Parquet" : "DuckDB";
}

const char *exportFormatExtension(ExportFormat format)
{
    return format == ExportFormat::Parquet ? ".parquet" : ".duckdb";
}

std::optional<ExportFormat> exportFormatFromPath(const std::filesystem::path &path)
{
    const std::string ext = lowerExtension(path);
    if (ext == ".parquet")
        return ExportFormat::Parquet;
    if (ext == ".duckdb" || ext == ".ddb")
        return ExportFormat::DuckDb;
    return std::nullopt;
}

std::optional<size_t> exportTextFile(const std::filesystem::path &source, const std::filesystem::path &target,
                                     ExportFormat format, const OpenOptions &options, std::stop_token stop,
                                     const std::function<void(size_t rows)> &progress)
{
    std::unique_ptr<DataSource> data_source = BackendRegistry::builtin().open(source, options);
    if (!data_source->isTextFormat())
        throw std::runtime_error(std::string("Only delimited text files can be converted to ") +
                                 exportFormatName(format));
    const auto sheet_names = data_source->sheetNames();
    std::unique_ptr<SheetSource> sheet = sheet_names.empty() ? nullptr : data_source->openSheet(sheet_names.front());
    if (!sheet)
        throw std::runtime_error("No table to convert");

    // Written next to the target, so that renaming it into place is atomic.
    // Declared before the database, which must close the file before it is
    // removed (Windows).
    TempFileGuard guard(target);
    const std::filesystem::path partial = guard.path();

    Staging staging(sheet->columns().size(), stop);
    size_t rows = 0;
    while (!stop.stop_requested())
    {
        const RowBlockPtr block = sheet->read(rows, BLOCK_ROWS, stop);
        for (const auto &row : block->rows)
            staging.append(row);
        rows += block->rows.size();
        if (progress)
            progress(rows);
        if (block->rows.size() < BLOCK_ROWS)
            break;
    }
    staging.close();
    if (stop.stop_requested())
        return std::nullopt;

    // Header names known after reading (ragged sources may report more later).
    const std::vector<ColumnInfo> header = sheet->columns();
    std::vector<std::string> header_names;
    std::vector<size_t> columns;
    for (size_t i = 0; i < staging.width(); ++i)
    {
        header_names.push_back(i < header.size() ? header[i].name : std::string());
        columns.push_back(i);
    }
    const std::string select = Staging::select(columns, uniqueNames(header_names), staging.types());
    if (stop.stop_requested())
        return std::nullopt;

    if (format == ExportFormat::Parquet)
    {
        staging.run("COPY (" + select + ") TO " + quoteSqlString(utf8String(partial)) +
                    " (FORMAT parquet, COMPRESSION zstd)");
    }
    else
    {
        staging.run("ATTACH " + quoteSqlString(utf8String(partial)) + " AS export_target");
        staging.run("CREATE TABLE export_target.main." + quoteSqlIdentifier(exportTableName(source)) + " AS " + select);
        // Checkpoints and closes the file, so that it is complete without a WAL.
        staging.run("DETACH export_target");
    }
    if (stop.stop_requested())
        return std::nullopt;
    if (format == ExportFormat::DuckDb)
    {
        // A WAL left by an old database would be replayed into the new one.
        std::error_code ignored;
        std::filesystem::remove(withSuffix(target, ".wal"), ignored);
    }
    std::filesystem::rename(partial, target);
    return rows;
}

std::filesystem::path defaultExportPath(const std::filesystem::path &source, ExportFormat format)
{
    std::filesystem::path name = stripCompressionExtension(source.filename());
    name.replace_extension(exportFormatExtension(format));
    return source.parent_path() / name;
}

std::string exportTableName(const std::filesystem::path &source)
{
    const std::string name = utf8String(stripCompressionExtension(source.filename()).stem());
    return name.empty() ? "data" : name;
}

const char *viewExportFormatName(ViewExportFormat format)
{
    switch (format)
    {
    case ViewExportFormat::Csv:
        return "CSV";
    case ViewExportFormat::Tsv:
        return "TSV";
    case ViewExportFormat::Excel:
        return "Excel";
    case ViewExportFormat::Parquet:
        break;
    }
    return "Parquet";
}

const char *viewExportFormatExtension(ViewExportFormat format)
{
    switch (format)
    {
    case ViewExportFormat::Csv:
        return ".csv";
    case ViewExportFormat::Tsv:
        return ".tsv";
    case ViewExportFormat::Excel:
        return ".xlsx";
    case ViewExportFormat::Parquet:
        break;
    }
    return ".parquet";
}

std::optional<ViewExportFormat> viewExportFormatFromPath(const std::filesystem::path &path)
{
    const std::string ext = lowerExtension(path);
    if (ext == ".csv")
        return ViewExportFormat::Csv;
    if (ext == ".tsv" || ext == ".tab")
        return ViewExportFormat::Tsv;
    if (ext == ".xlsx")
        return ViewExportFormat::Excel;
    if (ext == ".parquet")
        return ViewExportFormat::Parquet;
    return std::nullopt;
}

std::optional<size_t> exportView(const std::shared_ptr<TableModel> &model, const std::filesystem::path &target,
                                 ViewExportFormat format, const ViewExportOptions &options, std::stop_token stop,
                                 const std::function<void(size_t rows)> &progress)
{
    std::error_code ignored;
    if (!options.source.empty() && std::filesystem::equivalent(options.source, target, ignored))
        throw std::runtime_error("The file being viewed cannot be overwritten");

    // Written next to the target, so that renaming it into place is atomic.
    TempFileGuard guard(target);
    const std::filesystem::path partial = guard.path();

    std::unique_ptr<ViewReader> reader = model->readView();
    std::optional<size_t> rows;
    if (format == ViewExportFormat::Csv || format == ViewExportFormat::Tsv)
        rows = writeDelimited(*model, *reader, partial, format == ViewExportFormat::Csv ? ',' : '\t', options.hidden,
                              stop, progress);
    else
        rows = writeTyped(*model, *reader, partial, format, options.hidden, stop, progress);
    reader.reset();
    if (!rows)
        return std::nullopt;
    std::filesystem::rename(partial, target);
    return rows;
}

std::filesystem::path defaultViewExportPath(const std::filesystem::path &source, const std::string &sheet,
                                            ViewExportFormat format)
{
    std::string name = utf8String(stripCompressionExtension(source.filename()).stem());
    if (name.empty())
        name = "data";
    if (!sheet.empty())
    {
        name += '-';
        // Characters that file names cannot hold on some platform.
        for (const char c : sheet)
            name += std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos ||
                            static_cast<unsigned char>(c) < 0x20
                        ? '_'
                        : c;
    }
    name += "-export";
    name += viewExportFormatExtension(format);
    return source.parent_path() / std::filesystem::path(std::u8string(name.begin(), name.end()));
}
