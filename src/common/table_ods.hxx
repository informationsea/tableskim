#ifndef _TABLE_ODS_HXX_
#define _TABLE_ODS_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// OpenDocument spreadsheet (.ods) backend. One sheet per table. The
// workbook's content.xml is streamed out of the ZIP archive and parsed with
// expat: once when the file is opened (sheet names, row counts and merged
// cells, which the model needs before the rows), then again for each sheet
// opened, whose rows are converted as they are read.
std::unique_ptr<DataSource> openOdsDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerOdsBackend(BackendRegistry &registry);

#endif // _TABLE_ODS_HXX_
