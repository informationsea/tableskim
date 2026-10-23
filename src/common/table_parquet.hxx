#ifndef _TABLE_PARQUET_HXX_
#define _TABLE_PARQUET_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// Parquet backend: a single sheet (named after the file) read through DuckDB's
// read_parquet(), with the same paging and sort/filter pushdown as the DuckDB
// backend. In SQL (executeSql()) the file is a view named after it without
// the extension: data.parquet -> data.
std::unique_ptr<DataSource> openParquetDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerParquetBackend(BackendRegistry &registry);

#endif // _TABLE_PARQUET_HXX_
