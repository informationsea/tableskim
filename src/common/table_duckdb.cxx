#include "table_duckdb.hxx"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <duckdb.hpp>
#include <duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp>
#include <duckdb/common/local_file_system.hpp>
#include <duckdb/common/virtual_file_system.hpp>
#include <duckdb/parser/statement/create_statement.hpp>
#include <duckdb/parser/statement/drop_statement.hpp>

#include "sql_text.hxx"

// In-memory databases spill to ".tmp" in the current directory by default,
// which may be unwritable (e.g. "/" for a macOS app) or the user's files.
// Each instance gets its own directory, so instances never share files.
std::filesystem::path duckDbTempDirectory()
{
    static std::atomic<uint64_t> counter{0};
    static const uint64_t process_tag = []
    {
        std::random_device device;
        return (static_cast<uint64_t>(device()) << 32) ^ device();
    }();
    char name[64];
    std::snprintf(name, sizeof(name), "tableskim-duckdb-%016llx-%llu",
                  static_cast<unsigned long long>(process_tag), static_cast<unsigned long long>(counter++));
    return std::filesystem::temp_directory_path() / name;
}

namespace
{
    constexpr const char *kAttachedName = "tableskim_src";

    void registerFunctions(duckdb::Connection &connection);

    std::string quoteIdentifier(const std::string &name)
    {
        std::string quoted = "\"";
        for (const char c : name)
        {
            quoted += c;
            if (c == '"')
                quoted += '"';
        }
        return quoted + "\"";
    }

    std::string utf8Path(const std::filesystem::path &path)
    {
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }

    std::filesystem::path reserveTempDirectory()
    {
        for (;;)
        {
            auto path = duckDbTempDirectory();
            if (std::filesystem::create_directory(path))
                return path;
        }
    }

    // External readers remain usable, but only DuckDB's private spill area is writable.
    class ProtectedFileSystem final : public duckdb::LocalFileSystem
    {
        std::filesystem::path writable_directory_;

        void checkWrite(const std::string &path) const
        {
            std::error_code error;
            const auto resolved = std::filesystem::weakly_canonical(
                std::filesystem::path(std::u8string(path.begin(), path.end())), error);
            const auto mismatch = std::mismatch(writable_directory_.begin(), writable_directory_.end(),
                                                resolved.begin(), resolved.end());
            if (error || mismatch.first != writable_directory_.end() ||
                (std::filesystem::exists(resolved, error) && std::filesystem::is_regular_file(resolved, error) &&
                 std::filesystem::hard_link_count(resolved, error) > 1) || error)
                throw duckdb::PermissionException("SQL cannot write external files; use Export View instead");
        }

    public:
        duckdb::unique_ptr<duckdb::FileHandle> OpenFile(
            const std::string &path, duckdb::FileOpenFlags flags,
            duckdb::optional_ptr<duckdb::FileOpener> opener = nullptr) override
        {
            if (flags.OpenForWriting() || flags.CreateFileIfNotExists() || flags.OverwriteExistingFile() ||
                flags.OpenForAppending())
                checkWrite(path);
            return LocalFileSystem::OpenFile(path, flags, opener);
        }

        explicit ProtectedFileSystem(const std::filesystem::path &directory)
            : writable_directory_(std::filesystem::canonical(directory))
        {
        }

        void CreateDirectory(const std::string &path, duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            checkWrite(path);
            LocalFileSystem::CreateDirectory(path, opener);
        }

        void RemoveDirectory(const std::string &path, duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            checkWrite(path);
            LocalFileSystem::RemoveDirectory(path, opener);
        }

        void MoveFile(const std::string &source, const std::string &target,
                      duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            checkWrite(source);
            checkWrite(target);
            LocalFileSystem::MoveFile(source, target, opener);
        }

        void RemoveFile(const std::string &path, duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            checkWrite(path);
            LocalFileSystem::RemoveFile(path, opener);
        }

        bool TryRemoveFile(const std::string &path, duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            checkWrite(path);
            return LocalFileSystem::TryRemoveFile(path, opener);
        }

        void RemoveFiles(const duckdb::vector<duckdb::string> &paths,
                         duckdb::optional_ptr<duckdb::FileOpener> opener) override
        {
            for (const auto &path : paths)
                checkWrite(path);
            LocalFileSystem::RemoveFiles(paths, opener);
        }
    };

    void restrictSqlFunctions(duckdb::Connection &connection)
    {
        connection.BeginTransaction();
        auto &catalog = duckdb::Catalog::GetSystemCatalog(*connection.context);
        // Logging table functions bypass SET/lock_configuration, including when
        // called through a view or query(). Reject them before changing any state.
        for (const char *name : {"enable_logging", "disable_logging", "truncate_duckdb_logs"})
        {
            auto &entry = catalog.GetEntry<duckdb::TableFunctionCatalogEntry>(*connection.context, DEFAULT_SCHEMA, name);
            for (auto &function : entry.functions.functions)
                function.bind = [](duckdb::ClientContext &, duckdb::TableFunctionBindInput &,
                                   duckdb::vector<duckdb::LogicalType> &, duckdb::vector<duckdb::string> &)
                    -> duckdb::unique_ptr<duckdb::FunctionData>
                { throw duckdb::PermissionException("SQL cannot change logging settings"); };
        }
        connection.Commit();
    }

    struct Database
    {
        std::filesystem::path temp_directory;
        std::unique_ptr<duckdb::DuckDB> db;
        std::unique_ptr<duckdb::Connection> conn;
        size_t next_snapshot_id = 0;

        Database() : temp_directory(reserveTempDirectory())
        {
            try
            {
                duckdb::DBConfig config;
                // Only the statically linked extensions are used; never download any.
                config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
                config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
                config.file_system = duckdb::make_uniq<duckdb::VirtualFileSystem>(
                    duckdb::make_uniq<ProtectedFileSystem>(temp_directory));
                config.SetOptionByName("temp_directory", duckdb::Value(utf8Path(temp_directory)));
                config.SetOptionByName("allow_persistent_secrets", duckdb::Value::BOOLEAN(false));
                config.SetOptionByName("allow_community_extensions", duckdb::Value::BOOLEAN(false));
                db = std::make_unique<duckdb::DuckDB>(nullptr, &config);
                conn = std::make_unique<duckdb::Connection>(*db);
                registerFunctions(*conn);
                restrictSqlFunctions(*conn);
                query("SET lock_configuration = true");
            }
            catch (...)
            {
                conn.reset();
                db.reset();
                std::error_code ignored;
                std::filesystem::remove_all(temp_directory, ignored);
                throw;
            }
        }

        ~Database()
        {
            conn.reset();
            db.reset();
            // DuckDB removes the directory it created; this catches leftovers.
            std::error_code ignored;
            std::filesystem::remove_all(temp_directory, ignored);
        }

        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;

        std::unique_ptr<duckdb::MaterializedQueryResult> query(const std::string &sql)
        {
            auto result = conn->Query(sql);
            if (result->HasError())
                throw std::runtime_error(result->GetError());
            return result;
        }
    };

    // How a column takes part in filters and sorting.
    enum class ColumnKind
    {
        Number,
        Boolean,
        Text,
        Other, // compared by its text form
    };

    struct Column
    {
        std::string alias; // c0, c1, ...: avoids quoting and duplicate-name issues
        ColumnKind kind = ColumnKind::Other;
        std::string select; // expression converted to a type readColumn() handles
        bool temporal = false;
        bool floating = false;
        std::string temporal_number; // finite dates: days; timestamps: microseconds since the epoch
        std::string temporal_micros; // finite dates (their midnight) and timestamps: microseconds
    };

    Column describeColumn(size_t index, const duckdb::LogicalType &type)
    {
        using Id = duckdb::LogicalTypeId;
        Column column;
        column.alias = "c" + std::to_string(index);
        column.select = column.alias;
        switch (type.id())
        {
        case Id::TINYINT:
        case Id::SMALLINT:
        case Id::INTEGER:
        case Id::BIGINT:
        case Id::UTINYINT:
        case Id::USMALLINT:
        case Id::UINTEGER:
        case Id::UBIGINT:
            column.kind = ColumnKind::Number;
            break;
        case Id::FLOAT:
        case Id::DOUBLE:
            column.kind = ColumnKind::Number;
            column.floating = true;
            break;
        case Id::DECIMAL:
            column.kind = ColumnKind::Number;
            column.floating = true;
            column.select = "CAST(" + column.alias + " AS DOUBLE)";
            break;
        case Id::HUGEINT:
        case Id::UHUGEINT:
            // Kept exact as text; still sorted and filtered as numbers.
            column.kind = ColumnKind::Number;
            column.select = "CAST(" + column.alias + " AS VARCHAR)";
            break;
        case Id::BOOLEAN:
            column.kind = ColumnKind::Boolean;
            break;
        case Id::VARCHAR:
            column.kind = ColumnKind::Text;
            // JSON values (e.g. read_json() for a key whose values differ in
            // type): strings without their quotes, anything else as JSON.
            if (type.HasAlias() && type.GetAlias() == "JSON")
                column.select = "(" + column.alias + " ->> '$')";
            break;
        case Id::DATE:
            column.temporal = true;
            column.temporal_number = "date_diff('day', DATE '1970-01-01', " + column.alias + ")";
            break;
        case Id::TIMESTAMP:
        case Id::TIMESTAMP_TZ: // shown in UTC
            column.temporal = true;
            break;
        case Id::TIMESTAMP_SEC:
        case Id::TIMESTAMP_MS:
        case Id::TIMESTAMP_NS:
            column.temporal = true;
            column.select = "CAST(" + column.alias + " AS TIMESTAMP)";
            break;
        default: // TIME, INTERVAL, BLOB, UUID, ENUM, LIST, STRUCT, ...
            column.kind = ColumnKind::Text;
            column.select = "CAST(" + column.alias + " AS VARCHAR)";
            break;
        }
        // Text compares byte by byte, as in memory: a column's collation (say
        // NOCASE) or default_collation would make sorts, filters and GROUP BY
        // ignore case.
        if (column.kind == ColumnKind::Text || type.id() == Id::HUGEINT || type.id() == Id::UHUGEINT)
            column.select = "(" + column.select + " COLLATE \"binary\")";
        if (column.temporal && column.temporal_number.empty())
            column.temporal_number = "epoch_us(" + column.select + ")";
        if (column.temporal)
            column.temporal_micros = type.id() == Id::DATE ? "(" + column.temporal_number + ") * 86400000000"
                                                           : column.temporal_number;
        return column;
    }

    // ---------------------------------------------------------------------
    // Result conversion
    // ---------------------------------------------------------------------

    template <typename T, typename Convert>
    void readVector(duckdb::Vector &vector, size_t count, std::vector<TableRow> &rows, size_t first, size_t column,
                    Convert convert)
    {
        const T *data = duckdb::FlatVector::GetData<T>(vector);
        const auto &validity = duckdb::FlatVector::Validity(vector);
        for (size_t i = 0; i < count; ++i)
        {
            if (validity.RowIsValid(i))
                rows[first + i][column] = convert(data[i]);
        }
    }

    void readColumn(duckdb::Vector &vector, size_t count, std::vector<TableRow> &rows, size_t first, size_t column)
    {
        using Id = duckdb::LogicalTypeId;
        auto integer = [](auto v) -> CellValue
        { return static_cast<int64_t>(v); };
        switch (vector.GetType().id())
        {
        case Id::BOOLEAN:
            return readVector<bool>(vector, count, rows, first, column, [](bool v) -> CellValue
                                    { return v; });
        case Id::TINYINT:
            return readVector<int8_t>(vector, count, rows, first, column, integer);
        case Id::SMALLINT:
            return readVector<int16_t>(vector, count, rows, first, column, integer);
        case Id::INTEGER:
            return readVector<int32_t>(vector, count, rows, first, column, integer);
        case Id::BIGINT:
            return readVector<int64_t>(vector, count, rows, first, column, integer);
        case Id::UTINYINT:
            return readVector<uint8_t>(vector, count, rows, first, column, integer);
        case Id::USMALLINT:
            return readVector<uint16_t>(vector, count, rows, first, column, integer);
        case Id::UINTEGER:
            return readVector<uint32_t>(vector, count, rows, first, column, integer);
        case Id::UBIGINT:
            return readVector<uint64_t>(vector, count, rows, first, column, [](uint64_t v) -> CellValue
                                        {
                if (v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    return static_cast<int64_t>(v);
                // CellValue has no uint64_t alternative; text preserves every digit.
                return std::to_string(v); });
        case Id::FLOAT:
            return readVector<float>(vector, count, rows, first, column, [](float v) -> CellValue
                                     { return static_cast<double>(v); });
        case Id::DOUBLE:
            return readVector<double>(vector, count, rows, first, column, [](double v) -> CellValue
                                      { return v; });
        case Id::VARCHAR:
            return readVector<duckdb::string_t>(vector, count, rows, first, column, [](duckdb::string_t v) -> CellValue
                                                { return v.GetString(); });
        case Id::DATE:
            return readVector<duckdb::date_t>(vector, count, rows, first, column, [](duckdb::date_t v) -> CellValue
                                              {
                if (!duckdb::Date::IsFinite(v))
                    return duckdb::Date::ToString(v); // "infinity" / "-infinity"
                return Date{v.days}; });
        case Id::TIMESTAMP:
        case Id::TIMESTAMP_TZ:
            return readVector<duckdb::timestamp_t>(vector, count, rows, first, column, [](duckdb::timestamp_t v) -> CellValue
                                                   {
                if (!duckdb::Timestamp::IsFinite(v))
                    return duckdb::Timestamp::ToString(v);
                return DateTime{v.value}; });
        default:
            // Not reached: describeColumn() casts everything else to VARCHAR.
            for (size_t i = 0; i < count; ++i)
            {
                const duckdb::Value value = vector.GetValue(i);
                if (!value.IsNull())
                    rows[first + i][column] = value.ToString();
            }
            return;
        }
    }

    // ---------------------------------------------------------------------
    // ViewSpec -> SQL, mirroring the in-memory semantics in table.cxx:
    // empty strings count as empty, numbers compare numerically (also inside
    // text columns), everything else compares by its text bytewise, and empty
    // values sort last in both directions.
    // ---------------------------------------------------------------------

    std::string_view trimNumber(std::string_view text)
    {
        size_t begin = text.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return {};
        const size_t end = text.find_last_not_of(" \t") + 1;
        if (text[begin] == '+')
            ++begin;
        return text.substr(begin, end - begin);
    }

    void registerFunctions(duckdb::Connection &connection)
    {
        using duckdb::LogicalType;
        // The residual preserves exact integers within a rounded double's
        // bucket, while still allowing integer/float comparisons and ties.
        connection.CreateVectorizedFunction("tableskim_numeric_key", {LogicalType::VARCHAR},
            LogicalType::STRUCT({{"number", LogicalType::DOUBLE}, {"residual", LogicalType::BIGINT}}),
            [](duckdb::DataChunk &args, duckdb::ExpressionState &, duckdb::Vector &result)
            {
                duckdb::UnifiedVectorFormat input;
                args.data[0].ToUnifiedFormat(args.size(), input);
                const auto *text = input.GetData<duckdb::string_t>();
                result.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
                auto &validity = duckdb::FlatVector::Validity(result);
                validity.SetAllValid(args.size());
                auto &entries = duckdb::StructVector::GetEntries(result);
                for (auto &entry : entries)
                {
                    entry->SetVectorType(duckdb::VectorType::FLAT_VECTOR);
                    duckdb::FlatVector::Validity(*entry).SetAllValid(args.size());
                }
                auto *numbers = duckdb::FlatVector::GetData<double>(*entries[0]);
                auto *residuals = duckdb::FlatVector::GetData<int64_t>(*entries[1]);
                for (size_t i = 0; i < args.size(); ++i)
                {
                    const auto index = input.sel->get_index(i);
                    if (!input.validity.RowIsValid(index))
                    {
                        duckdb::FlatVector::SetNull(result, i, true);
                        continue;
                    }
                    const std::string_view trimmed = trimNumber({text[index].GetData(), text[index].GetSize()});
                    if (trimmed.empty())
                    {
                        duckdb::FlatVector::SetNull(result, i, true);
                        continue;
                    }
                    int64_t integer = 0;
                    const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), integer);
                    residuals[i] = 0;
                    if (parsed.ec == std::errc() && parsed.ptr == trimmed.data() + trimmed.size())
                    {
                        numbers[i] = static_cast<double>(integer);
                        if (numbers[i] >= 0x1p63)
                            residuals[i] = (integer - std::numeric_limits<int64_t>::max()) - 1;
                        else
                            residuals[i] = integer - static_cast<int64_t>(numbers[i]);
                    }
                    else
                    {
                        if (const auto number = parseDouble(trimmed))
                            numbers[i] = *number;
                        else
                            duckdb::FlatVector::SetNull(result, i, true);
                    }
                }
            });

        // parseTemporal() for text cells: microseconds, or NULL if not a date.
        connection.CreateVectorizedFunction("tableskim_temporal_key", {LogicalType::VARCHAR}, LogicalType::BIGINT,
            [](duckdb::DataChunk &args, duckdb::ExpressionState &, duckdb::Vector &result)
            {
                duckdb::UnaryExecutor::ExecuteWithNulls<duckdb::string_t, int64_t>(args.data[0], result, args.size(),
                    [](duckdb::string_t text, duckdb::ValidityMask &mask, duckdb::idx_t index)
                    {
                        if (const auto value = parseTemporal({text.GetData(), text.GetSize()}))
                            return value->micros;
                        mask.SetInvalid(index);
                        return int64_t{0};
                    });
            });

        connection.CreateVectorizedFunction("tableskim_format_number", {LogicalType::DOUBLE}, LogicalType::VARCHAR,
            [](duckdb::DataChunk &args, duckdb::ExpressionState &, duckdb::Vector &result)
            {
                duckdb::UnaryExecutor::Execute<double, duckdb::string_t>(args.data[0], result, args.size(),
                    [&](double value)
                    {
                        return duckdb::StringVector::AddString(result, formatCell(value));
                    });
            });

        connection.CreateVectorizedFunction("tableskim_format_temporal", {LogicalType::DATE}, LogicalType::VARCHAR,
            [](duckdb::DataChunk &args, duckdb::ExpressionState &, duckdb::Vector &result)
            {
                duckdb::UnaryExecutor::Execute<duckdb::date_t, duckdb::string_t>(args.data[0], result, args.size(),
                    [&](duckdb::date_t value)
                    {
                        const std::string text = duckdb::Date::IsFinite(value) ? formatCell(Date{value.days}) :
                                                                             duckdb::Date::ToString(value);
                        return duckdb::StringVector::AddString(result, text);
                    });
            });
        for (const auto &type : {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP_TZ})
        {
            connection.CreateVectorizedFunction("tableskim_format_temporal", {type}, LogicalType::VARCHAR,
                [](duckdb::DataChunk &args, duckdb::ExpressionState &, duckdb::Vector &result)
                {
                    duckdb::UnaryExecutor::Execute<duckdb::timestamp_t, duckdb::string_t>(args.data[0], result, args.size(),
                        [&](duckdb::timestamp_t value)
                        {
                            const std::string text = duckdb::Timestamp::IsFinite(value) ? formatCell(DateTime{value.value}) :
                                                                                        duckdb::Timestamp::ToString(value);
                            return duckdb::StringVector::AddString(result, text);
                        });
                });
        }
    }

    std::optional<std::string> integerLiteral(const std::string &text)
    {
        const std::string_view trimmed = trimNumber(text);
        if (trimmed.empty())
            return std::nullopt;
        int64_t integer = 0;
        const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), integer);
        if (parsed.ec == std::errc() && parsed.ptr == trimmed.data() + trimmed.size())
            return std::to_string(integer);
        uint64_t unsigned_integer = 0;
        const auto unsigned_parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), unsigned_integer);
        if (unsigned_parsed.ec == std::errc() && unsigned_parsed.ptr == trimmed.data() + trimmed.size())
            return std::to_string(unsigned_integer);
        return std::nullopt;
    }

    const char *sqlOperator(FilterOp op)
    {
        switch (op)
        {
        case FilterOp::Equals:
            return "=";
        case FilterOp::NotEquals:
            return "<>";
        case FilterOp::Less:
            return "<";
        case FilterOp::LessEqual:
            return "<=";
        case FilterOp::Greater:
            return ">";
        case FilterOp::GreaterEqual:
            return ">=";
        default:
            return "=";
        }
    }

    const char *sqlBool(bool value) { return value ? "TRUE" : "FALSE"; }

    // Whether an empty cell (NULL or '') matches the condition.
    bool emptyMatches(const FilterCondition &condition)
    {
        return cellMatchesCondition(CellValue{}, condition);
    }

    std::string textExpression(const Column &column)
    {
        if (column.temporal)
            return "coalesce(tableskim_format_temporal(" + column.select + "), '')";
        if (column.kind == ColumnKind::Boolean)
            return "(CASE WHEN " + column.alias + " THEN 'TRUE' WHEN NOT " + column.alias + " THEN 'FALSE' ELSE '' END)";
        // FLOAT is widened and DECIMAL is rounded to double when readColumn() displays them.
        if (column.floating)
            return "coalesce(tableskim_format_number(CAST(" + column.alias + " AS DOUBLE)), '')";
        return "coalesce(CAST(" + column.select + " AS VARCHAR), '')";
    }

    std::string emptyExpression(const Column &column)
    {
        if (column.kind == ColumnKind::Text)
            return "(" + column.select + " IS NULL OR " + column.select + " = '')";
        return "(" + column.alias + " IS NULL)";
    }

    // FilterOp::IsNumber, as isNumberCell() reads what readColumn() returns.
    std::string numberExpression(const Column &column)
    {
        if (column.kind == ColumnKind::Number)
            return column.floating ? "coalesce(isfinite(CAST(" + column.alias + " AS DOUBLE)), FALSE)"
                                   : "(" + column.alias + " IS NOT NULL)";
        if (column.kind == ColumnKind::Text)
            return "coalesce(isfinite(struct_extract(tableskim_numeric_key(" + textExpression(column) +
                   "), 'number')), FALSE)";
        return "FALSE"; // booleans, dates and times
    }

    // FilterOp::IsDate, as isDateCell(); infinite dates are read as text.
    std::string dateExpression(const Column &column)
    {
        if (column.temporal)
            return "coalesce(isfinite(" + column.select + "), FALSE)";
        if (column.kind == ColumnKind::Text)
        {
            const std::string text = textExpression(column);
            return "(tableskim_numeric_key(" + text + ") IS NULL AND tableskim_temporal_key(" + text +
                   ") IS NOT NULL)";
        }
        return "FALSE";
    }

    std::string foldCaseExpression(const std::string &text)
    {
        // Match the ASCII-only folding used by cellMatchesCondition() and search.
        return "translate(" + text + ", 'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')";
    }

    // A comparison of numbers (numeric text too) numerically, else of text.
    std::string untypedComparison(const Column &column, const FilterCondition &condition)
    {
        const std::string text = textExpression(column);
        const std::string value = duckDbStringLiteral(condition.value);
        const std::string op = sqlOperator(condition.op);
        const std::string text_compare = "(" + text + " " + op + " " + value + ")";
        if (!parseNumber(condition.value))
            return text_compare;

        // Preserve native integer comparisons, including unsigned values beyond
        // int64_t that CellValue can only display as text.
        if (column.kind == ColumnKind::Number && !column.floating)
        {
            if (const auto integer = integerLiteral(condition.value))
                return "coalesce(" + column.alias + " " + op + " " + *integer + ", " +
                       sqlBool(emptyMatches(condition)) + ")";
        }

        std::string numeric_text = text;
        if (column.temporal)
            numeric_text = "(CASE WHEN isfinite(" + column.select + ") THEN CAST(" + column.temporal_number +
                           " AS VARCHAR) ELSE " + text + " END)";
        else if (column.kind == ColumnKind::Boolean)
            numeric_text = "CAST(CAST(" + column.alias + " AS INTEGER) AS VARCHAR)";
        // Keep floats as doubles: their shortest display text can look like an
        // integer that differs from the exact binary value. DECIMAL is displayed
        // as double too. Integer text uses the same keys as the filter value.
        const std::string as_number = column.floating
            ? "(CASE WHEN " + column.alias + " IS NULL THEN NULL ELSE struct_pack(number := CAST(" +
                  column.alias + " AS DOUBLE), residual := 0::BIGINT) END)"
            : "tableskim_numeric_key(" + numeric_text + ")";
        return "(CASE WHEN " + as_number + " IS NOT NULL THEN " + as_number + " " + op +
               " tableskim_numeric_key(" + value + ") ELSE " + text_compare + " END)";
    }

    // A date or date-time value against the cell's instant, NULL if it has
    // none (NULL, infinity, text that is not a date): a date value covers
    // its whole day.
    std::string temporalComparison(const std::string &instant, const TemporalValue &value, FilterOp op)
    {
        const std::string start = std::to_string(value.micros);
        const std::string end = std::to_string(value.micros + (value.has_time ? 1 : 86400000000LL));
        switch (op)
        {
        case FilterOp::Equals:
            return "(" + instant + " >= " + start + " AND " + instant + " < " + end + ")";
        case FilterOp::NotEquals:
            return "(" + instant + " < " + start + " OR " + instant + " >= " + end + ")";
        case FilterOp::Less:
            return "(" + instant + " < " + start + ")";
        case FilterOp::LessEqual:
            return "(" + instant + " < " + end + ")";
        case FilterOp::Greater:
            return "(" + instant + " >= " + end + ")";
        default:
            return "(" + instant + " >= " + start + ")";
        }
    }

    std::string conditionExpression(const std::vector<Column> &columns, const FilterCondition &condition)
    {
        if (condition.column >= columns.size())
            return sqlBool(emptyMatches(condition));
        const Column &column = columns[condition.column];
        const std::string text = textExpression(column);
        const std::string value = duckDbStringLiteral(condition.value);

        switch (condition.op)
        {
        case FilterOp::IsEmpty:
            return emptyExpression(column);
        case FilterOp::IsNotEmpty:
            return "(NOT " + emptyExpression(column) + ")";
        case FilterOp::Contains:
            return "contains(" + foldCaseExpression(text) + ", " + foldCaseExpression(value) + ")";
        case FilterOp::NotContains:
            return "(NOT contains(" + foldCaseExpression(text) + ", " + foldCaseExpression(value) + "))";
        case FilterOp::Matches:
            // RE2 as in memory; filterError() has checked the pattern.
            return "regexp_matches(" + text + ", " + value + ", 'i')";
        case FilterOp::NotMatches:
            return "(NOT regexp_matches(" + text + ", " + value + ", 'i'))";
        case FilterOp::IsNumber:
            return numberExpression(column);
        case FilterOp::IsDate:
            return dateExpression(column);
        default:
            break;
        }

        if (const auto temporal = parseTemporal(condition.value))
        {
            std::string instant;
            if (column.temporal)
                instant = "(CASE WHEN isfinite(" + column.select + ") THEN " + column.temporal_micros + " END)";
            else if (column.kind == ColumnKind::Text || column.kind == ColumnKind::Other)
                instant = "tableskim_temporal_key(" + text + ")"; // read as text, as in memory
            if (!instant.empty())
                return "(CASE WHEN " + instant + " IS NOT NULL THEN " +
                       temporalComparison(instant, *temporal, condition.op) + " ELSE " +
                       untypedComparison(column, condition) + " END)";
        }
        if (column.kind == ColumnKind::Boolean)
        {
            if (const auto boolean = parseBoolean(condition.value))
                return "coalesce(CAST(" + column.alias + " AS INTEGER) " + sqlOperator(condition.op) + " " +
                       (*boolean ? "1" : "0") + ", " + sqlBool(emptyMatches(condition)) + ")";
        }
        return untypedComparison(column, condition);
    }

    std::string whereClause(const std::vector<Column> &columns, const std::optional<Filter> &filter)
    {
        if (!filter || filter->conditions.empty())
            return {};
        const char *combine = filter->combine == Filter::Combine::And ? " AND " : " OR ";
        std::string sql = " WHERE ";
        for (size_t i = 0; i < filter->conditions.size(); ++i)
        {
            if (i > 0)
                sql += combine;
            sql += conditionExpression(columns, filter->conditions[i]);
        }
        return sql;
    }

    std::string orderByClause(const std::vector<Column> &columns, const std::vector<SortKey> &sort,
                              const std::string &order_column)
    {
        std::vector<std::string> terms;
        for (const auto &key : sort)
        {
            if (key.column >= columns.size())
                continue;
            const Column &column = columns[key.column];
            const std::string dir = key.ascending ? " ASC" : " DESC";
            terms.push_back(emptyExpression(column) + " ASC");
            if (column.kind == ColumnKind::Text)
            {
                // Numbers before text (reversed when descending), as in compareCells().
                const std::string as_number = "tableskim_numeric_key(" + column.select + ")";
                terms.push_back("(" + as_number + " IS NULL)" + dir);
                terms.push_back(as_number + dir);
                terms.push_back("(CASE WHEN " + as_number + " IS NULL AND NOT " + emptyExpression(column) +
                                " THEN " + column.select + " END)" + dir);
            }
            else
                terms.push_back(column.alias + dir);
        }
        // Unsorted scans already keep table order (preserve_insertion_order),
        // and ORDER BY would make every deep page sort the whole relation.
        if (terms.empty())
            return {};
        if (!order_column.empty())
            terms.push_back("t." + order_column + " ASC");
        std::string sql = " ORDER BY ";
        for (size_t i = 0; i < terms.size(); ++i)
        {
            if (i > 0)
                sql += ", ";
            sql += terms[i];
        }
        return sql;
    }

    // ---------------------------------------------------------------------
    // Sources
    // ---------------------------------------------------------------------

    class DuckDbSheetSource : public SheetSource
    {
    private:
        std::shared_ptr<Database> db_;
        DuckDbRelation relation_;
        std::vector<ColumnInfo> column_info_;
        std::vector<Column> columns_;
        std::string select_;
        std::string from_;
        std::string where_;
        std::string order_by_;
        std::vector<SortKey> sort_;
        std::optional<size_t> count_; // for the current filter
        std::string snapshot_table_;

        bool ensureSnapshot(std::stop_token stop)
        {
            if (!relation_.order_column.empty())
                return true;
            // A view has no persistent row identity. Evaluate it once so that
            // both paging and repeated sorts share the same rows and tie order.
            const std::string name = quoteIdentifier("tableskim_snapshot_" + std::to_string(db_->next_snapshot_id++));
            if (!query("CREATE TEMP TABLE " + name + " AS SELECT * FROM " + from_, stop))
                return false;
            snapshot_table_ = name;
            from_ = name + " AS t";
            relation_.order_column = "rowid";
            relation_.cheap_count = true;
            order_by_ = orderByClause(columns_, sort_, relation_.order_column);
            return true;
        }

        std::unique_ptr<duckdb::MaterializedQueryResult> query(const std::string &sql, std::stop_token stop)
        {
            std::stop_callback interrupt(stop, [this]
                                         { db_->conn->Interrupt(); });
            auto result = db_->conn->Query(sql);
            if (result->HasError())
            {
                if (stop.stop_requested())
                    return nullptr;
                throw std::runtime_error(result->GetError());
            }
            return result;
        }

        std::optional<size_t> count(std::stop_token stop)
        {
            auto result = query("SELECT count(*) FROM " + from_ + where_, stop);
            if (!result)
                return std::nullopt;
            return static_cast<size_t>(result->GetValue(0, 0).GetValue<int64_t>());
        }

    public:
        DuckDbSheetSource(std::shared_ptr<Database> db, DuckDbRelation relation)
            : db_(std::move(db)), relation_(std::move(relation))
        {
            auto result = db_->query("SELECT * FROM " + relation_.from_sql + " LIMIT 0");
            std::string aliases;
            for (size_t i = 0; i < result->types.size(); ++i)
            {
                column_info_.push_back({result->names[i]});
                columns_.push_back(describeColumn(i, result->types[i]));
                if (i > 0)
                {
                    aliases += ", ";
                    select_ += ", ";
                }
                aliases += columns_.back().alias;
                select_ += columns_.back().select;
            }
            if (columns_.empty())
                throw std::runtime_error("No columns: " + relation_.sheet_name);
            from_ = relation_.from_sql + " AS t(" + aliases + ")";
            order_by_ = orderByClause(columns_, {}, relation_.order_column);
        }

        ~DuckDbSheetSource() override
        {
            if (!snapshot_table_.empty())
            {
                try
                {
                    db_->query("DROP TABLE IF EXISTS " + snapshot_table_);
                }
                catch (...)
                {
                }
            }
        }

        SourceCapabilities capabilities() const override { return {.random_access = true, .query_pushdown = true}; }
        std::vector<ColumnInfo> columns() override { return column_info_; }

        RowCount rowCount() override
        {
            if (!count_ && where_.empty() && relation_.cheap_count)
                count_ = count({});
            if (count_)
                return {*count_, RowCount::Kind::Exact};
            return {};
        }

        std::optional<size_t> countRowsSlow(std::stop_token stop) override
        {
            if (!ensureSnapshot(stop))
                return std::nullopt;
            if (!count_)
                count_ = count(stop);
            return count_;
        }

        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token stop) override
        {
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            if (!ensureSnapshot(stop))
                return block;
            auto result = query("SELECT " + select_ + " FROM " + from_ + where_ + order_by_ + " LIMIT " +
                                    std::to_string(max_rows) + " OFFSET " + std::to_string(first_row),
                                stop);
            if (!result)
                return block;
            block->rows.reserve(result->RowCount());
            for (auto chunk = result->Fetch(); chunk && chunk->size() > 0; chunk = result->Fetch())
            {
                chunk->Flatten();
                const size_t n = chunk->size();
                const size_t first = block->rows.size();
                block->rows.resize(first + n, TableRow(columns_.size()));
                for (size_t c = 0; c < columns_.size(); ++c)
                    readColumn(chunk->data[c], n, block->rows, first, c);
            }
            return block;
        }

        std::optional<size_t> columnValueCounts(size_t column, const ValueCountSink &add,
                                                std::stop_token stop) override
        {
            if (column >= columns_.size())
                return std::nullopt;
            // The same expression as read(), so that values convert alike.
            auto result = query("SELECT " + columns_[column].select + ", count(*) FROM " + from_ + where_ +
                                    " GROUP BY 1 ORDER BY 2 DESC",
                                stop);
            if (!result)
                return std::nullopt;
            size_t distinct = 0;
            std::vector<TableRow> values;
            for (auto chunk = result->Fetch(); chunk && chunk->size() > 0; chunk = result->Fetch())
            {
                if (stop.stop_requested())
                    return std::nullopt;
                chunk->Flatten();
                const size_t n = chunk->size();
                values.assign(n, TableRow(1));
                readColumn(chunk->data[0], n, values, 0, 0);
                const auto *counts = duckdb::FlatVector::GetData<int64_t>(chunk->data[1]);
                for (size_t i = 0; i < n; ++i)
                {
                    add(values[i][0], static_cast<size_t>(counts[i]));
                    // NULL and '' are not distinct values (and may be two groups).
                    if (!isEmptyCell(values[i][0]))
                        ++distinct;
                }
            }
            return distinct;
        }

        void applyView(const ViewSpec &spec) override
        {
            std::string where = whereClause(columns_, spec.filter);
            if (where != where_)
                count_.reset();
            where_ = std::move(where);
            sort_ = spec.sort;
            order_by_ = orderByClause(columns_, sort_, relation_.order_column);
        }
    };

    // TEMP view behind the sheet of the last query run with executeSql().
    constexpr const char *kSqlResultView = "tableskim_sql_result";

    void checkSqlStatement(const duckdb::SQLStatement &statement)
    {
        using Type = duckdb::StatementType;
        using Catalog = duckdb::CatalogType;
        switch (statement.type)
        {
        case Type::SELECT_STATEMENT:
        case Type::INSERT_STATEMENT:
        case Type::UPDATE_STATEMENT:
        case Type::DELETE_STATEMENT:
            return;
        case Type::CREATE_STATEMENT:
        {
            const auto &info = *statement.Cast<duckdb::CreateStatement>().info;
            if (info.temporary && (info.type == Catalog::TABLE_ENTRY || info.type == Catalog::VIEW_ENTRY))
                return;
            break;
        }
        case Type::DROP_STATEMENT:
        {
            const auto &info = *statement.Cast<duckdb::DropStatement>().info;
            if (info.type == Catalog::TABLE_ENTRY || info.type == Catalog::VIEW_ENTRY)
                return;
            break;
        }
        default:
            break;
        }
        throw std::runtime_error("SQL allows queries and temporary table/view operations only; use Export View to save files");
    }

    void prepareUserSql(Database &db, const std::string &sql)
    {
        auto prepared = db.conn->Prepare(sql);
        if (prepared->HasError())
            throw std::runtime_error(prepared->GetError());
        for (const auto &[name, modification] : prepared->GetStatementProperties().modified_databases)
        {
            if (name != TEMP_CATALOG)
                throw std::runtime_error("SQL may only modify temporary tables and views");
        }
    }

    std::string describeResult(duckdb::MaterializedQueryResult &result)
    {
        // INSERT, UPDATE, DELETE and CREATE TABLE AS report a count.
        if (result.ColumnCount() == 1 && result.names[0] == "Count" && result.RowCount() == 1)
            return result.GetValue(0, 0).ToString() + " rows changed";
        if (result.RowCount() > 0)
            return "Done; " + std::to_string(result.RowCount()) + " rows returned (only a single query is shown as a sheet)";
        return "Done";
    }

    class DuckDbDataSource : public DataSource
    {
    private:
        std::shared_ptr<Database> db_;
        std::vector<DuckDbRelation> relations_;
        bool supports_sql_;
        std::string sql_sheet_; // chosen on the first query

    public:
        DuckDbDataSource(std::shared_ptr<Database> db, std::vector<DuckDbRelation> relations, bool supports_sql)
            : db_(std::move(db)), relations_(std::move(relations)), supports_sql_(supports_sql)
        {
        }

        bool supportsSql() const override { return supports_sql_; }

        SqlOutcome executeSql(const std::string &sql, std::stop_token stop) override
        {
            if (!supports_sql_)
                return DataSource::executeSql(sql, stop);
            std::stop_callback interrupt(stop, [this]
                                         { db_->conn->Interrupt(); });
            if (stop.stop_requested())
                throw std::runtime_error("SQL execution was cancelled");
            duckdb::vector<duckdb::unique_ptr<duckdb::SQLStatement>> statements;
            try
            {
                statements = db_->conn->ExtractStatements(sql);
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(duckdb::ErrorData(e).Message());
            }
            if (statements.empty())
                throw std::runtime_error("No SQL statement");
            // Check the entire batch before running any statement, including wrappers
            // such as EXPLAIN, PREPARE and CALL that could hide another operation.
            for (const auto &statement : statements)
                checkSqlStatement(*statement);
            if (statements.size() == 1 && statements[0]->type == duckdb::StatementType::SELECT_STATEMENT)
            {
                const auto &statement = *statements[0];
                const std::string text = statement.stmt_length > 0
                                             ? trimSqlStatement(std::string_view(sql).substr(statement.stmt_location, statement.stmt_length))
                                             : trimSqlStatement(sql);
                // Binding the query alone reports errors against the user's text,
                // not the CREATE VIEW around it.
                prepareUserSql(*db_, text);
                if (stop.stop_requested())
                    throw std::runtime_error("SQL execution was cancelled");
                db_->query("CREATE OR REPLACE TEMP VIEW " + std::string(kSqlResultView) + " AS " + text);
                if (sql_sheet_.empty())
                {
                    sql_sheet_ = uniqueSheetName(SQL_RESULT_SHEET, sheetNames());
                    relations_.push_back({.sheet_name = sql_sheet_,
                                          .from_sql = "temp.main." + std::string(kSqlResultView)});
                }
                return {.sheet = sql_sheet_};
            }
            std::unique_ptr<duckdb::MaterializedQueryResult> result;
            for (const auto &statement : statements)
            {
                if (stop.stop_requested())
                    throw std::runtime_error("SQL execution was cancelled");
                const auto text = statement->ToString();
                prepareUserSql(*db_, text);
                if (stop.stop_requested())
                    throw std::runtime_error("SQL execution was cancelled");
                result = db_->query(text);
            }
            return {.message = describeResult(*result)};
        }

        std::vector<std::string> sheetNames() override
        {
            std::vector<std::string> names;
            for (const auto &relation : relations_)
                names.push_back(relation.sheet_name);
            return names;
        }

        std::vector<SqlTable> sqlSchema() override
        {
            if (!supports_sql_)
                return {};
            // The file's (the default database) and temporary ones; snapshots
            // and the query view are TableSkim's own.
            auto result = db_->query(
                "SELECT database_name = 'temp' OR schema_name = current_schema(), schema_name, table_name, "
                "column_name FROM duckdb_columns() "
                "WHERE NOT internal AND database_name IN (current_database(), 'temp') "
                "AND table_name NOT LIKE 'tableskim\\_%' ESCAPE '\\' "
                "ORDER BY database_name = 'temp', schema_name <> current_schema(), schema_name, table_name, "
                "column_index");
            std::vector<SqlTable> tables;
            for (size_t row = 0; row < result->RowCount(); ++row)
            {
                const bool default_schema = result->GetValue(0, row).GetValue<bool>();
                const std::string name = (default_schema ? std::string() : result->GetValue(1, row).ToString() + ".") +
                                         result->GetValue(2, row).ToString();
                if (tables.empty() || tables.back().name != name)
                    tables.push_back({name, {}});
                tables.back().columns.push_back(result->GetValue(3, row).ToString());
            }
            return tables;
        }

        std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) override
        {
            const auto it = std::find_if(relations_.begin(), relations_.end(), [&](const DuckDbRelation &relation)
                                         { return relation.sheet_name == sheet_name; });
            if (it == relations_.end())
                return nullptr;
            return std::make_unique<DuckDbSheetSource>(db_, *it);
        }
    };

    bool hasMagic(std::span<const std::byte> head, size_t offset, std::string_view magic)
    {
        if (head.size() < offset + magic.size())
            return false;
        for (size_t i = 0; i < magic.size(); ++i)
        {
            if (head[offset + i] != static_cast<std::byte>(magic[i]))
                return false;
        }
        return true;
    }
} // namespace

std::string escapeDuckDbGlob(const std::string &path)
{
    std::string result;
    for (const char c : path)
    {
        if (c == '*' || c == '?' || c == '[')
            result += std::string("[") + c + "]";
        else
            result += c;
    }
    return result;
}

std::string duckDbStringLiteral(const std::string &text)
{
    std::string quoted = "'";
    for (const char c : text)
    {
        quoted += c;
        if (c == '\'')
            quoted += '\'';
    }
    return quoted + "'";
}

std::unique_ptr<DataSource> openDuckDbRelations(std::vector<DuckDbRelation> relations)
{
    auto db = std::make_shared<Database>();
    bool supports_sql = false;
    for (const auto &relation : relations)
    {
        db->query("SELECT * FROM " + relation.from_sql + " LIMIT 0");
        if (!relation.sql_name.empty())
        {
            db->query("CREATE TEMP VIEW " + quoteIdentifier(relation.sql_name) + " AS SELECT * FROM " + relation.from_sql);
            supports_sql = true;
        }
    }
    return std::make_unique<DuckDbDataSource>(std::move(db), std::move(relations), supports_sql);
}

std::unique_ptr<DataSource> openDuckDbDataSource(const std::filesystem::path &path, const OpenOptions &)
{
    try
    {
        auto db = std::make_shared<Database>();
        db->query("ATTACH " + duckDbStringLiteral(utf8Path(path)) + " AS " + kAttachedName + " (READ_ONLY)");
        const std::string database = duckDbStringLiteral(kAttachedName);
        auto result = db->query(
            "SELECT schema_name, name, is_table FROM ("
            "SELECT schema_name, table_name AS name, TRUE AS is_table FROM duckdb_tables() "
            "WHERE database_name = " + database + " AND NOT internal "
            "UNION ALL "
            "SELECT schema_name, view_name, FALSE FROM duckdb_views() "
            "WHERE database_name = " + database + " AND NOT internal) "
            "ORDER BY is_table DESC, schema_name <> 'main', schema_name, name");

        std::vector<DuckDbRelation> relations;
        for (size_t row = 0; row < result->RowCount(); ++row)
        {
            const std::string schema = result->GetValue(0, row).ToString();
            const std::string name = result->GetValue(1, row).ToString();
            const bool is_table = result->GetValue(2, row).GetValue<bool>();
            // As documented (file-formats.md), names may coincide (table b of
            // schema a and table "a.b" of main): the sheet is the first one,
            // and SQL reaches the other.
            relations.push_back({
                .sheet_name = schema == "main" ? name : schema + "." + name,
                .from_sql = std::string(kAttachedName) + "." + quoteIdentifier(schema) + "." + quoteIdentifier(name),
                .order_column = is_table ? "rowid" : "",
                .cheap_count = is_table,
            });
        }
        // Lets queries name the file's tables without the attachment alias.
        db->query("USE " + std::string(kAttachedName));
        return std::make_unique<DuckDbDataSource>(std::move(db), std::move(relations), true);
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string("Cannot read DuckDB file: ") + e.what());
    }
}

void registerDuckDbBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "DuckDB",
        .probe = [](const std::filesystem::path &, std::span<const std::byte> head) -> int
        {
            // The main header follows an 8-byte checksum.
            return hasMagic(head, 8, "DUCK") ? 100 : 0;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openDuckDbDataSource(path, options); },
    });
}
