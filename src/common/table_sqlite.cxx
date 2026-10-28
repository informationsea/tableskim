#include "table_sqlite.hxx"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string_view>

#include <sqlite3.h>

#include "sql_text.hxx"

namespace
{
    using StatementPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

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

    std::string stringLiteral(const std::string &text)
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

    std::string utf8Path(const std::filesystem::path &path)
    {
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }

    bool equalsIgnoreCase(std::string_view a, std::string_view b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y)
                          {
            auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(x) == lower(y); });
    }

    CellValue toCell(sqlite3_value *value)
    {
        switch (sqlite3_value_type(value))
        {
        case SQLITE_INTEGER:
            return static_cast<int64_t>(sqlite3_value_int64(value));
        case SQLITE_FLOAT:
            return sqlite3_value_double(value);
        case SQLITE_TEXT:
        {
            const auto *text = reinterpret_cast<const char *>(sqlite3_value_text(value));
            return std::string(text, static_cast<size_t>(sqlite3_value_bytes(value)));
        }
        case SQLITE_BLOB:
            return "BLOB (" + std::to_string(sqlite3_value_bytes(value)) + " bytes)";
        default:
            return {};
        }
    }

    // ---------------------------------------------------------------------
    // SQL functions mirroring the in-memory sort/filter semantics in table.cxx
    // ---------------------------------------------------------------------

    // tableskim_match(x, op, value): cellMatchesCondition() for one cell.
    void matchFunction(sqlite3_context *context, int, sqlite3_value **argv)
    {
        FilterCondition condition;
        condition.op = static_cast<FilterOp>(sqlite3_value_int(argv[1]));
        condition.value = formatCell(toCell(argv[2]));
        sqlite3_result_int(context, cellMatchesCondition(toCell(argv[0]), condition) ? 1 : 0);
    }

    using NumericValue = std::variant<int64_t, double>;

    std::optional<NumericValue> numericValue(sqlite3_value *value)
    {
        switch (sqlite3_value_type(value))
        {
        case SQLITE_INTEGER:
            return static_cast<int64_t>(sqlite3_value_int64(value));
        case SQLITE_FLOAT:
            return sqlite3_value_double(value);
        case SQLITE_TEXT:
            break;
        default:
            return std::nullopt;
        }
        const auto *data = reinterpret_cast<const char *>(sqlite3_value_text(value));
        const std::string text(data, static_cast<size_t>(sqlite3_value_bytes(value)));
        std::string_view trimmed = text;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.remove_suffix(1);
        if (!trimmed.empty() && trimmed.front() == '+')
            trimmed.remove_prefix(1);
        int64_t integer = 0;
        const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), integer);
        if (!trimmed.empty() && parsed.ec == std::errc() && parsed.ptr == trimmed.data() + trimmed.size())
            return integer;
        if (const auto number = parseNumber(text))
            return *number;
        return std::nullopt;
    }

    // tableskim_number(x): the numeric value used for sorting (numbers inside
    // text included), or NULL. SQLite compares INTEGER and REAL exactly.
    void numberFunction(sqlite3_context *context, int, sqlite3_value **argv)
    {
        if (const auto number = numericValue(argv[0]))
        {
            if (const auto *integer = std::get_if<int64_t>(&*number))
                sqlite3_result_int64(context, *integer);
            else
                sqlite3_result_double(context, std::get<double>(*number));
        }
        else
            sqlite3_result_null(context);
    }

    // SQLite converts NaN results to NULL, so classify them separately from
    // nonnumeric text. NaNs tie and sort after other numbers, before text.
    void sortKindFunction(sqlite3_context *context, int, sqlite3_value **argv)
    {
        int kind = 2;
        if (const auto number = numericValue(argv[0]))
        {
            const auto *floating = std::get_if<double>(&*number);
            kind = floating && std::isnan(*floating) ? 1 : 0;
        }
        sqlite3_result_int(context, kind);
    }

    // tableskim_text(x): formatCell() of the value, compared bytewise.
    void textFunction(sqlite3_context *context, int, sqlite3_value **argv)
    {
        const std::string text = formatCell(toCell(argv[0]));
        sqlite3_result_text(context, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    }

    // `data`: a bool, whether transactions are allowed (only TableSkim's own).
    int authorizeSql(void *data, int action, const char *first, const char *second,
                     const char *database, const char *)
    {
        const auto isTemp = [](const char *name)
        { return name && sqlite3_stricmp(name, "temp") == 0; };
        switch (action)
        {
        case SQLITE_SELECT:
        case SQLITE_READ:
        case SQLITE_RECURSIVE:
        case SQLITE_CREATE_TEMP_TABLE:
        case SQLITE_CREATE_TEMP_VIEW:
        case SQLITE_CREATE_TEMP_INDEX:
        case SQLITE_DROP_TEMP_TABLE:
        case SQLITE_DROP_TEMP_VIEW:
        case SQLITE_DROP_TEMP_INDEX:
            return SQLITE_OK;
        case SQLITE_INSERT:
        case SQLITE_UPDATE:
        case SQLITE_DELETE:
        case SQLITE_CREATE_TABLE:
        case SQLITE_CREATE_VIEW:
        case SQLITE_CREATE_INDEX:
        case SQLITE_ANALYZE:
        case SQLITE_REINDEX:
            return isTemp(database) ? SQLITE_OK : SQLITE_DENY;
        case SQLITE_ALTER_TABLE:
            return isTemp(first) ? SQLITE_OK : SQLITE_DENY;
        case SQLITE_TRANSACTION:
        case SQLITE_SAVEPOINT:
            // A query's BEGIN would keep the file locked, and its ROLLBACK
            // would drop the TEMP tables the sheets read from.
            return *static_cast<const bool *>(data) ? SQLITE_OK : SQLITE_DENY;
        case SQLITE_FUNCTION:
            // File I/O functions are not registered here, and must stay unavailable
            // even with a system SQLite build or a future additional extension.
            for (const char *name : {"load_extension", "writefile", "edit"})
                if (second && sqlite3_stricmp(second, name) == 0)
                    return SQLITE_DENY;
            return SQLITE_OK;
        case SQLITE_PRAGMA:
            // These PRAGMAs only inspect metadata, even when given an argument.
            for (const char *name : {"table_info", "table_xinfo", "table_list", "index_info", "index_xinfo",
                                     "index_list", "foreign_key_list", "database_list", "compile_options",
                                     "function_list", "module_list", "pragma_list", "collation_list"})
                if (first && sqlite3_stricmp(first, name) == 0)
                    return SQLITE_OK;
            return SQLITE_DENY;
        default:
            // Includes ATTACH (also used by VACUUM INTO), DETACH and persistent DDL.
            return SQLITE_DENY;
        }
    }

    struct Database
    {
        sqlite3 *db = nullptr;
        size_t next_temp_id = 0;
        bool transactions_allowed = false; // see authorizeSql()

        explicit Database(const std::filesystem::path &path)
        {
            const int rc = sqlite3_open_v2(utf8Path(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
            if (rc != SQLITE_OK)
            {
                const std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
                sqlite3_close(db);
                throw std::runtime_error(message);
            }
            constexpr int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
            sqlite3_create_function(db, "tableskim_match", 3, flags, nullptr, matchFunction, nullptr, nullptr);
            sqlite3_create_function(db, "tableskim_number", 1, flags, nullptr, numberFunction, nullptr, nullptr);
            sqlite3_create_function(db, "tableskim_sort_kind", 1, flags, nullptr, sortKindFunction, nullptr, nullptr);
            sqlite3_create_function(db, "tableskim_text", 1, flags, nullptr, textFunction, nullptr, nullptr);
            // Keep this installed while stepping, including lazy result views and
            // statements SQLite recompiles after a schema change.
            sqlite3_set_authorizer(db, authorizeSql, &transactions_allowed);
        }

        ~Database() { sqlite3_close_v2(db); }
        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;

        StatementPtr prepare(const std::string &sql)
        {
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db));
            return {stmt, sqlite3_finalize};
        }

        // false when the statement was interrupted
        // A transaction statement of TableSkim's own (see authorizeSql()).
        bool execTransaction(const std::string &sql)
        {
            transactions_allowed = true;
            try
            {
                const bool done = exec(sql);
                transactions_allowed = false;
                return done;
            }
            catch (...)
            {
                transactions_allowed = false;
                throw;
            }
        }

        bool exec(const std::string &sql)
        {
            char *error = nullptr;
            const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
            const std::string message = error ? error : "";
            sqlite3_free(error);
            if (rc == SQLITE_INTERRUPT)
                return false;
            if (rc != SQLITE_OK)
                throw std::runtime_error(message);
            return true;
        }

        // true: a row is available; false: done or interrupted
        bool step(sqlite3_stmt *stmt)
        {
            const int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW)
                return true;
            if (rc == SQLITE_DONE || rc == SQLITE_INTERRUPT)
                return false;
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    };

    // Lets the worker's stop request abort a long query (sqlite3_interrupt is
    // thread-safe and a no-op when nothing is running).
    class InterruptScope
    {
    private:
        std::stop_callback<std::function<void()>> callback_;

    public:
        InterruptScope(std::stop_token stop, sqlite3 *db)
            : callback_(std::move(stop), std::function<void()>([db]
                                                               { sqlite3_interrupt(db); }))
        {
        }
    };

    struct Relation
    {
        std::string name; // the sheet name; in SQL schema.name
        bool is_table = false;
        std::string schema = "main";
    };

    // TEMP view behind the sheet of the last query run with executeSql().
    constexpr const char *kSqlResultView = "tableskim_sql_result";

    class SqliteSheetSource : public SheetSource
    {
    private:
        std::shared_ptr<Database> db_;
        std::string name_;
        std::vector<ColumnInfo> columns_;
        std::vector<std::string> refs_; // column expressions on alias t
        std::string select_;           // refs_ joined
        std::string with_;             // CTE renaming the columns of views / WITHOUT ROWID tables
        std::string from_;             // "<table> AS t" or "src AS t"
        std::string rowid_;            // usable as t.<rowid_>; empty without a rowid
        bool is_table_ = false;        // COUNT(*) is cheap enough to run when opening
        std::optional<size_t> table_count_;
        std::string snapshot_table_; // fixed rows and rowids for views / query results

        ViewSpec spec_;
        std::string view_table_; // TEMP table of the current view; empty until built
        size_t view_count_ = 0;

        // Unsorted rowid tables: continue after the previous block without OFFSET.
        size_t next_row_ = 0;
        std::optional<int64_t> next_after_rowid_;

        std::string conditionExpression(const FilterCondition &condition) const
        {
            const std::string cell = condition.column < refs_.size() ? refs_[condition.column] : "NULL";
            return "tableskim_match(" + cell + ", " + std::to_string(static_cast<int>(condition.op)) + ", " +
                   stringLiteral(condition.value) + ")";
        }

        std::string whereClause() const
        {
            if (!spec_.filter || spec_.filter->empty())
                return {};
            const char *combine = spec_.filter->combine == Filter::Combine::And ? " AND " : " OR ";
            std::string sql = " WHERE (";
            if (spec_.filter->conditions.empty())
                sql += "TRUE";
            for (size_t i = 0; i < spec_.filter->conditions.size(); ++i)
            {
                if (i > 0)
                    sql += combine;
                const auto &condition = spec_.filter->conditions[i];
                sql += conditionExpression(condition);
            }
            sql += ")";
            for (const auto &condition : spec_.filter->narrowing)
                sql += " AND (" + conditionExpression(condition) + ")";
            return sql;
        }

        // Empty values last; numbers before text (reversed when descending);
        // ties keep the table order, as the in-memory stable sort does.
        std::string orderTerms(const std::string &tie) const
        {
            std::string sql;
            for (const auto &key : spec_.sort)
            {
                if (key.column >= refs_.size())
                    continue;
                const std::string &cell = refs_[key.column];
                const std::string number = "tableskim_number(" + cell + ")";
                const std::string kind = "tableskim_sort_kind(" + cell + ")";
                const char *dir = key.ascending ? " ASC, " : " DESC, ";
                sql += "(" + cell + " IS NULL OR " + cell + " = '') ASC, ";
                sql += kind + dir;
                sql += number + dir;
                sql += "(CASE WHEN " + kind + " = 2 THEN tableskim_text(" + cell + ") END)" + dir;
            }
            return sql + tie;
        }

        bool ensureSnapshot(std::stop_token stop)
        {
            if (!rowid_.empty())
                return true;
            if (stop.stop_requested())
                return false;
            const std::string table = "temp." + quoteIdentifier("tableskim_snapshot_" + std::to_string(db_->next_temp_id++));
            std::string columns;
            for (size_t i = 0; i < refs_.size(); ++i)
                columns += (i > 0 ? ", c" : "c") + std::to_string(i);
            InterruptScope interrupt(stop, db_->db);
            bool done = false;
            try
            {
                // No column affinity: keep INTEGER, REAL and TEXT values as
                // returned. A volatile query must run only once, across pages,
                // counts and later sorts/filters.
                done = db_->exec("CREATE TABLE " + table + " (" + columns + "); INSERT INTO " + table + " " +
                                 with_ + "SELECT * FROM src");
            }
            catch (...)
            {
                db_->exec("DROP TABLE IF EXISTS " + table);
                throw;
            }
            if (!done)
            {
                db_->exec("DROP TABLE IF EXISTS " + table);
                return false;
            }
            snapshot_table_ = table;
            from_ = table + " AS t";
            with_.clear();
            rowid_ = "rowid";
            table_count_.reset();
            return true;
        }

        bool ensureView(std::stop_token stop)
        {
            if (!view_table_.empty())
                return true;
            if (!ensureSnapshot(stop) || stop.stop_requested())
                return false;
            const std::string table = "temp." + quoteIdentifier("tableskim_view_" + std::to_string(db_->next_temp_id++));
            const std::string sql = "CREATE TABLE " + table + " (pos INTEGER PRIMARY KEY, src INTEGER); "
                                    "INSERT INTO " + table + " SELECT row_number() OVER (ORDER BY " +
                                    orderTerms("t." + rowid_) + "), t." + rowid_ + " FROM " + from_ + whereClause() + ";";
            InterruptScope interrupt(stop, db_->db);
            bool done = false;
            try
            {
                done = db_->exec(sql);
            }
            catch (...)
            {
                db_->exec("DROP TABLE IF EXISTS " + table);
                throw;
            }
            if (!done)
            {
                db_->exec("DROP TABLE IF EXISTS " + table);
                return false;
            }
            auto count = db_->prepare("SELECT count(*) FROM " + table);
            db_->step(count.get());
            view_count_ = static_cast<size_t>(sqlite3_column_int64(count.get(), 0));
            view_table_ = table;
            return true;
        }

        void dropView()
        {
            if (!view_table_.empty())
                db_->exec("DROP TABLE IF EXISTS " + view_table_);
            view_table_.clear();
        }

        std::optional<size_t> countTable(std::stop_token stop)
        {
            if (!table_count_)
            {
                InterruptScope interrupt(stop, db_->db);
                auto stmt = db_->prepare(with_ + "SELECT count(*) FROM " + from_);
                if (stop.stop_requested() || !db_->step(stmt.get()))
                    return std::nullopt;
                table_count_ = static_cast<size_t>(sqlite3_column_int64(stmt.get(), 0));
            }
            return table_count_;
        }

    public:
        SqliteSheetSource(std::shared_ptr<Database> db, const Relation &relation)
            : db_(std::move(db)), name_(relation.name), is_table_(relation.is_table)
        {
            const std::string table = relation.schema + "." + quoteIdentifier(relation.schema == "main" ? name_ : kSqlResultView);
            auto stmt = db_->prepare("SELECT * FROM " + table);
            const int count = sqlite3_column_count(stmt.get());
            if (count == 0)
                throw std::runtime_error("No columns: " + name_);
            for (int i = 0; i < count; ++i)
            {
                const char *name = sqlite3_column_name(stmt.get(), i);
                columns_.push_back({name ? name : ""});
            }

            if (relation.is_table)
            {
                // rowid can be shadowed by a real column; WITHOUT ROWID tables have none.
                for (const char *candidate : {"rowid", "_rowid_", "oid"})
                {
                    const bool shadowed = std::any_of(columns_.begin(), columns_.end(), [&](const ColumnInfo &column)
                                                      { return equalsIgnoreCase(column.name, candidate); });
                    if (shadowed)
                        continue;
                    sqlite3_stmt *probe = nullptr;
                    const std::string sql = "SELECT t." + std::string(candidate) + " FROM " + table + " AS t";
                    if (sqlite3_prepare_v2(db_->db, sql.c_str(), -1, &probe, nullptr) == SQLITE_OK)
                        rowid_ = candidate;
                    sqlite3_finalize(probe);
                    break;
                }
            }

            if (!rowid_.empty())
            {
                from_ = table + " AS t";
                for (const auto &column : columns_)
                    refs_.push_back("t." + quoteIdentifier(column.name));
            }
            else
            {
                from_ = "src AS t";
                std::string aliases;
                for (size_t i = 0; i < columns_.size(); ++i)
                {
                    aliases += (i > 0 ? ", c" : "c") + std::to_string(i);
                    refs_.push_back("t.c" + std::to_string(i));
                }
                with_ = "WITH src(" + aliases + ") AS (SELECT * FROM " + table + ") ";
            }
            for (size_t i = 0; i < refs_.size(); ++i)
                select_ += (i > 0 ? ", " : "") + refs_[i];
        }

        ~SqliteSheetSource() override
        {
            try
            {
                dropView();
                if (!snapshot_table_.empty())
                    db_->exec("DROP TABLE IF EXISTS " + snapshot_table_);
            }
            catch (...)
            {
            }
        }

        SourceCapabilities capabilities() const override { return {.random_access = true, .query_pushdown = true}; }
        std::vector<ColumnInfo> columns() override { return columns_; }

        RowCount rowCount() override
        {
            if (!spec_.empty())
                return view_table_.empty() ? RowCount{} : RowCount{view_count_, RowCount::Kind::Exact};
            if (is_table_)
                countTable({}); // views are counted by countRowsSlow() instead
            if (table_count_)
                return {*table_count_, RowCount::Kind::Exact};
            return {};
        }

        std::optional<size_t> countRowsSlow(std::stop_token stop) override
        {
            if (!ensureSnapshot(stop))
                return std::nullopt;
            if (!spec_.empty())
                return ensureView(stop) ? std::optional<size_t>(view_count_) : std::nullopt;
            return countTable(stop);
        }

        RowBlockPtr read(size_t first_row, size_t max_rows, std::stop_token stop) override
        {
            auto block = std::make_shared<RowBlock>();
            block->first_row = first_row;
            if (!ensureSnapshot(stop))
                return block;

            StatementPtr stmt{nullptr, sqlite3_finalize};
            bool keyset = false;
            if (!spec_.empty())
            {
                if (!ensureView(stop))
                    return block;
                stmt = db_->prepare("SELECT " + select_ + " FROM " + view_table_ + " AS v JOIN " + from_ +
                                    " ON t." + rowid_ + " = v.src WHERE v.pos > ?1 AND v.pos <= ?2 ORDER BY v.pos");
                sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(first_row));
                sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(first_row + max_rows));
            }
            else
            {
                keyset = true;
                const std::string select = "SELECT " + select_ + ", t." + rowid_ + " FROM " + from_;
                if (next_after_rowid_ && first_row == next_row_)
                {
                    stmt = db_->prepare(select + " WHERE t." + rowid_ + " > ?1 ORDER BY t." + rowid_ + " LIMIT ?2");
                    sqlite3_bind_int64(stmt.get(), 1, *next_after_rowid_);
                    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(max_rows));
                }
                else
                {
                    stmt = db_->prepare(select + " ORDER BY t." + rowid_ + " LIMIT ?1 OFFSET ?2");
                    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(max_rows));
                    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(first_row));
                }
            }
            InterruptScope interrupt(stop, db_->db);
            const int columns = static_cast<int>(columns_.size());
            std::optional<int64_t> last_rowid;
            while (!stop.stop_requested() && db_->step(stmt.get()))
            {
                TableRow row(columns_.size());
                for (int c = 0; c < columns; ++c)
                    row[static_cast<size_t>(c)] = toCell(sqlite3_column_value(stmt.get(), c));
                block->rows.push_back(std::move(row));
                if (keyset)
                    last_rowid = sqlite3_column_int64(stmt.get(), columns);
            }
            if (stop.stop_requested())
            {
                block->rows.clear();
                return block;
            }
            if (last_rowid)
            {
                next_row_ = first_row + block->rows.size();
                next_after_rowid_ = last_rowid;
            }
            return block;
        }

        void applyView(const ViewSpec &spec) override
        {
            if (spec == spec_)
                return;
            dropView();
            spec_ = spec;
        }
    };

    class SqliteDataSource : public DataSource
    {
    private:
        std::shared_ptr<Database> db_;
        std::vector<Relation> relations_;
        std::string sql_sheet_; // chosen on the first query

        // Replaces the TEMP view; on failure the previous one is kept.
        void replaceResultView(const std::string &query)
        {
            const std::string view = "temp." + quoteIdentifier(kSqlResultView);
            db_->execTransaction("SAVEPOINT tableskim_sql");
            try
            {
                db_->exec("DROP VIEW IF EXISTS " + view);
                db_->exec("CREATE TEMP VIEW " + quoteIdentifier(kSqlResultView) + " AS " + query);
                // Some errors (e.g. unknown functions) show only when the view is used.
                db_->prepare("SELECT * FROM " + view);
                db_->execTransaction("RELEASE tableskim_sql");
            }
            catch (...)
            {
                db_->execTransaction("ROLLBACK TO tableskim_sql; RELEASE tableskim_sql");
                throw;
            }
        }

    public:
        explicit SqliteDataSource(const std::filesystem::path &path) : db_(std::make_shared<Database>(path))
        {
            // sqlite_master rather than sqlite_schema: works with older system SQLite.
            auto stmt = db_->prepare("SELECT name, type FROM sqlite_master WHERE type IN ('table', 'view') "
                                     "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY type = 'view', rowid");
            while (db_->step(stmt.get()))
            {
                const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
                const auto *type = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 1));
                relations_.push_back({name ? name : "", type && std::string_view(type) == "table"});
            }
        }

        std::vector<std::string> sheetNames() override
        {
            std::vector<std::string> names;
            for (const auto &relation : relations_)
                names.push_back(relation.name);
            return names;
        }

        std::unique_ptr<SheetSource> openSheet(const std::string &sheet_name) override
        {
            const auto it = std::find_if(relations_.begin(), relations_.end(), [&](const Relation &relation)
                                         { return relation.name == sheet_name; });
            if (it == relations_.end())
                return nullptr;
            return std::make_unique<SqliteSheetSource>(db_, *it);
        }

        bool supportsSql() const override { return true; }

        std::vector<SqlTable> sqlSchema() override
        {
            std::vector<SqlTable> tables;
            auto names = db_->prepare(
                "SELECT name FROM (SELECT name, type, 0 AS temp, rowid FROM sqlite_master "
                "UNION ALL SELECT name, type, 1, rowid FROM sqlite_temp_master) "
                "WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' "
                "AND name NOT LIKE 'tableskim\\_%' ESCAPE '\\' ORDER BY temp, type = 'view', rowid");
            while (db_->step(names.get()))
            {
                const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(names.get(), 0));
                tables.push_back({name ? name : "", {}});
            }
            for (auto &table : tables)
            {
                try
                {
                    auto columns = db_->prepare("SELECT name FROM pragma_table_info(" + stringLiteral(table.name) + ")");
                    while (db_->step(columns.get()))
                    {
                        const auto *column = reinterpret_cast<const char *>(sqlite3_column_text(columns.get(), 0));
                        table.columns.push_back(column ? column : "");
                    }
                }
                catch (const std::exception &)
                {
                    // A broken view: its name alone.
                }
            }
            return tables;
        }

        SqlOutcome executeSql(const std::string &sql, std::stop_token stop) override
        {
            InterruptScope interrupt(stop, db_->db);
            if (stop.stop_requested())
                throw std::runtime_error("SQL execution was cancelled");
            sqlite3_stmt *raw = nullptr;
            const char *tail = nullptr;
            if (sqlite3_prepare_v2(db_->db, sql.c_str(), static_cast<int>(sql.size()), &raw, &tail) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db_->db));
            StatementPtr stmt(raw, sqlite3_finalize);
            if (!stmt)
                throw std::runtime_error("No SQL statement");
            const bool single = isBlankSql(tail ? tail : "");
            const std::string keyword = firstSqlKeyword(sql);
            const bool query = keyword == "select" || keyword == "with" || keyword == "values";
            if (single && query && sqlite3_column_count(stmt.get()) > 0 && sqlite3_stmt_readonly(stmt.get()))
            {
                const std::string text = trimSqlStatement(sqlite3_sql(stmt.get()));
                stmt.reset();
                replaceResultView(text);
                if (sql_sheet_.empty())
                {
                    sql_sheet_ = uniqueSheetName(SQL_RESULT_SHEET, sheetNames());
                    relations_.push_back({.name = sql_sheet_, .is_table = false, .schema = "temp"});
                }
                return {.sheet = sql_sheet_};
            }
            stmt.reset();
            const int changes_before = sqlite3_total_changes(db_->db);
            if (!db_->exec(sql) || stop.stop_requested())
                throw std::runtime_error("SQL execution was cancelled");
            const int changes = sqlite3_total_changes(db_->db) - changes_before;
            return {.message = changes > 0 ? std::to_string(changes) + " rows changed" : "Done"};
        }
    };
} // namespace

std::unique_ptr<DataSource> openSqliteDataSource(const std::filesystem::path &path, const OpenOptions &)
{
    try
    {
        return std::make_unique<SqliteDataSource>(path);
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string("Cannot read SQLite file: ") + e.what());
    }
}

void registerSqliteBackend(BackendRegistry &registry)
{
    registry.add({
        .name = "SQLite",
        .probe = [](const std::filesystem::path &, std::span<const std::byte> head) -> int
        {
            constexpr std::string_view magic{"SQLite format 3\0", 16};
            if (head.size() < magic.size())
                return 0;
            for (size_t i = 0; i < magic.size(); ++i)
            {
                if (head[i] != static_cast<std::byte>(magic[i]))
                    return 0;
            }
            return 100;
        },
        .open = [](const std::filesystem::path &path, const OpenOptions &options)
        { return openSqliteDataSource(path, options); },
    });
}
