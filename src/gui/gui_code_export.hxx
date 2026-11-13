#ifndef _GUI_CODE_EXPORT_HXX_
#define _GUI_CODE_EXPORT_HXX_

#include <functional>
#include <memory>
#include <string>

#include "analysis_code.hxx"

class TableModel;

// Asked of a TableWindow by the chart windows ("Save Code..."): writes code
// repeating `analysis` on the model's view.
using CodeCallback = std::function<void(const std::shared_ptr<TableModel> &model, const CodeAnalysis &analysis)>;

// Asks where to save the code, in a file chooser whose filter picks the
// language (R, Python with polars, Python with pandas), and writes it there.
// `directory`: where the chooser starts the first time.
void saveAnalysisCode(const AnalysisCodeSpec &spec, const std::string &directory);

#endif // _GUI_CODE_EXPORT_HXX_
