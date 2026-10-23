#ifndef _TABLE_CSV_HXX_
#define _TABLE_CSV_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// CSV / TSV backend (sequential: rows are read from the start of the file).
// The delimiter is chosen from the extension (.csv -> ',', .tsv/.tab -> '\t')
// unless OpenOptions::delimiter is given; other extensions are guessed.
// gzip, bzip2, xz and zstd compressed files are decompressed on the fly; the
// delimiter then comes from the inner extension (data.tsv.gz -> '\t').
std::unique_ptr<DataSource> openCsvDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerCsvBackend(BackendRegistry &registry);

#endif // _TABLE_CSV_HXX_
