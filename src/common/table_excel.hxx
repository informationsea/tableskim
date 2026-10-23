#ifndef _TABLE_EXCEL_HXX_
#define _TABLE_EXCEL_HXX_

#include <filesystem>
#include <memory>

#include "table_source.hxx"

// Excel (.xlsx / .xlsm) backend based on OpenXLSX. One sheet per worksheet.
// OpenXLSX loads a whole worksheet XML into memory when the sheet is opened
// (on the worker thread); rows are then converted to cells sequentially.
std::unique_ptr<DataSource> openExcelDataSource(const std::filesystem::path &path, const OpenOptions &options = {});

void registerExcelBackend(BackendRegistry &registry);

#endif // _TABLE_EXCEL_HXX_
