#ifndef _TABLE_DUCKDB_HXX_
#define _TABLE_DUCKDB_HXX_

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "table_source.hxx"

// DuckDB database (.duckdb) backend. The file is attached read-only to an
// in-memory database; every table and view becomes a sheet ("name" for the
// main schema, "schema.name" otherwise). Rows are fetched with LIMIT/OFFSET
// and sort/filter are pushed down as SQL. Views are snapshotted lazily into
// temporary tables on the worker so that page boundaries have a stable order.
// executeSql() runs with the file as the default database.
std::unique_ptr<DataSource> openDuckDbDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerDuckDbBackend(BackendRegistry &registry);

// The same machinery for any relation DuckDB can query, so that other formats
// DuckDB reads natively (Parquet, ...) only have to name their relations.
struct DuckDbRelation
{
    std::string sheet_name;
    std::string from_sql;     // e.g. read_parquet('/path/file.parquet')
    std::string order_column; // stable unique row id (rowid, ...); empty snapshots the relation lazily
    bool cheap_count = false; // COUNT(*) without a filter is fast (tables, files with metadata)
    // Non-empty: a TEMP view of this name lets executeSql() query the relation,
    // and the source supports SQL.
    std::string sql_name;
};

// Opens an in-memory DuckDB and checks that every relation can be queried.
// Throws std::runtime_error.
std::unique_ptr<DataSource> openDuckDbRelations(std::vector<DuckDbRelation> relations);

// A new, not yet created directory for DuckDB to spill to (its temp_directory
// setting). In-memory databases otherwise use ".tmp" in the current directory.
std::filesystem::path duckDbTempDirectory();

// A path for the read_*() functions, which take a glob pattern without an
// escape: brackets make "*", "?" and "[" in it match only themselves.
std::string escapeDuckDbGlob(const std::string &path);

#endif // _TABLE_DUCKDB_HXX_
