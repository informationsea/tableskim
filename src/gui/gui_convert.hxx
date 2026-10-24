#ifndef _GUI_CONVERT_HXX_
#define _GUI_CONVERT_HXX_

#include <filesystem>
#include <memory>

#include "group_by.hxx"
#include "table_export.hxx"

// Runs exportTextFile() on a worker thread behind a modal progress dialog
// with a Cancel button. Returns true if the target was written; errors are
// reported to the user here.
bool runConversion(const std::filesystem::path &source, const std::filesystem::path &target, ExportFormat format,
                   const OpenOptions &options);

// Runs exportView() the same way.
bool runViewExport(const std::shared_ptr<TableModel> &model, const std::filesystem::path &target,
                   ViewExportFormat format, const ViewExportOptions &options);

// Runs groupByView() the same way; nullopt if it failed or was cancelled.
std::optional<ResultSheet> runGroupBy(const std::shared_ptr<TableModel> &model, const GroupBySpec &spec);

// Runs TableBook::saveEdits() behind a modal dialog, which cannot be
// cancelled. Errors are left to the caller.
TableBook::SaveResult runSave(TableBook &book, const std::filesystem::path &target);

#endif // _GUI_CONVERT_HXX_
