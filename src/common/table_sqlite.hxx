#ifndef _TABLE_SQLITE_HXX_
#define _TABLE_SQLITE_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// SQLite database backend. The file is opened read-only and every table and
// view becomes a sheet. Unsorted tables are paged by rowid; a sort/filter is
// evaluated once into a TEMP table of row positions, which later pages index.
// Filters call the in-memory semantics (cellMatchesCondition) as an SQL
// function, so results match the other backends exactly.
std::unique_ptr<DataSource> openSqliteDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerSqliteBackend(BackendRegistry &registry);

#endif // _TABLE_SQLITE_HXX_
