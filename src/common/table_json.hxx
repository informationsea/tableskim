#ifndef _TABLE_JSON_HXX_
#define _TABLE_JSON_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// JSON backend: a single sheet (named after the file) read through DuckDB's
// read_json(), with the same paging and sort/filter pushdown as the DuckDB
// backend. An array of objects and newline-delimited objects (NDJSON /
// JSON Lines) are both detected; object keys become columns, and nested
// objects and arrays are shown as text. gzip and zstd compression are read
// as well. In SQL the file is a view named after it: data.json.gz -> data.
std::unique_ptr<DataSource> openJsonDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerJsonBackend(BackendRegistry &registry);

#endif // _TABLE_JSON_HXX_
