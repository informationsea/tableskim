#include "web_session.hxx"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <stdexcept>

#include "cell_format.hxx"
#include "column_type.hxx"
#include "file_util.hxx"
#include "group_by.hxx"
#include "search_hits.hxx"
#include "table_export.hxx"
#include "text_encoding.hxx"
#include "web_json.hxx"

namespace
{
    // The most rows and columns one "rows" request returns.
    constexpr size_t MAX_ROWS = 1000;
    constexpr size_t MAX_COLUMNS = 500;
    // Cell text is cut there, as the GUI does when drawing.
    constexpr size_t MAX_CELL_BYTES = 256;
    // Most frequent values listed in statistics and histograms.
    constexpr size_t TOP_VALUES = 10;
    // Points of a scatter plot sent to the frontend, a sample of the rows.
    constexpr size_t SCATTER_POINTS = 20000;

    struct OpName
    {
        FilterOp op;
        const char *name;
    };
    constexpr OpName OP_NAMES[] = {
        {FilterOp::Equals, "equals"},
        {FilterOp::NotEquals, "notEquals"},
        {FilterOp::Contains, "contains"},
        {FilterOp::NotContains, "notContains"},
        {FilterOp::Less, "less"},
        {FilterOp::LessEqual, "lessEqual"},
        {FilterOp::Greater, "greater"},
        {FilterOp::GreaterEqual, "greaterEqual"},
        {FilterOp::IsEmpty, "isEmpty"},
        {FilterOp::IsNotEmpty, "isNotEmpty"},
        {FilterOp::Matches, "matches"},
        {FilterOp::NotMatches, "notMatches"},
        {FilterOp::IsNumber, "isNumber"},
        {FilterOp::IsDate, "isDate"},
    };

    const char *opName(FilterOp op)
    {
        for (const auto &entry : OP_NAMES)
        {
            if (entry.op == op)
                return entry.name;
        }
        return "contains";
    }

    FilterOp parseOp(const std::string &name)
    {
        for (const auto &entry : OP_NAMES)
        {
            if (name == entry.name)
                return entry.op;
        }
        throw std::invalid_argument("unknown filter operator: " + name);
    }

    unsigned eventBits(TableEvent::Kind kind)
    {
        switch (kind)
        {
        case TableEvent::Kind::RowsLoaded:
        case TableEvent::Kind::CellsChanged:
            return WebSession::ROWS;
        case TableEvent::Kind::RowCountChanged:
        case TableEvent::Kind::ColumnsChanged:
            return WebSession::ROW_COUNT;
        case TableEvent::Kind::ViewChanged:
            return WebSession::VIEW | WebSession::ROWS | WebSession::ROW_COUNT;
        case TableEvent::Kind::SearchProgress:
            return WebSession::SEARCH;
        case TableEvent::Kind::MemoryCheckpoint:
            return WebSession::CHECKPOINT;
        case TableEvent::Kind::Opened:
        case TableEvent::Kind::Error:
            return WebSession::STATE | WebSession::ROWS | WebSession::ROW_COUNT;
        case TableEvent::Kind::StatsProgress:
            return WebSession::STATS;
        case TableEvent::Kind::HistogramProgress:
            return WebSession::HISTOGRAM;
        case TableEvent::Kind::ScatterProgress:
            return WebSession::SCATTER;
        }
        return 0;
    }

    // A non-negative integer, or an error naming it.
    size_t jsonIndex(const JsonValue &value, std::string_view name)
    {
        const auto number = value.number();
        if (!number || *number < 0 || *number != std::floor(*number))
            throw std::invalid_argument("expected a non-negative integer: " + std::string(name));
        return static_cast<size_t>(std::min(*number, 9.0e15));
    }

    std::vector<size_t> jsonIndexes(const JsonValue &value, std::string_view name)
    {
        std::vector<size_t> indexes;
        if (const auto *array = value.array())
        {
            for (const JsonValue &entry : *array)
                indexes.push_back(jsonIndex(entry, name));
        }
        return indexes;
    }

    std::vector<SortKey> parseSort(const JsonValue &value)
    {
        std::vector<SortKey> sort;
        if (const auto *keys = value.array())
        {
            for (const JsonValue &key : *keys)
            {
                sort.push_back({jsonIndex(key["column"], "column"), key["ascending"].boolean().value_or(true)});
            }
        }
        return sort;
    }

    std::optional<Filter> parseFilter(const JsonValue &value)
    {
        if (value.isNull())
            return std::nullopt;
        Filter filter;
        if (const std::string *combine = value["combine"].string(); combine && *combine == "or")
            filter.combine = Filter::Combine::Or;
        if (const auto *conditions = value["conditions"].array())
        {
            for (const JsonValue &condition : *conditions)
            {
                const std::string *op = condition["op"].string();
                if (!op)
                    throw std::invalid_argument("expected a string: op");
                const std::string *text = condition["value"].string();
                filter.conditions.push_back({jsonIndex(condition["column"], "column"), parseOp(*op), text ? *text : ""});
            }
        }
        return filter;
    }

    void writeSort(JsonWriter &json, const std::vector<SortKey> &sort)
    {
        json.beginArray();
        for (const SortKey &key : sort)
            json.beginObject().key("column").value(key.column).key("ascending").value(key.ascending).endObject();
        json.endArray();
    }

    void writeView(JsonWriter &json, const ViewSpec &spec)
    {
        json.beginObject().key("sort");
        writeSort(json, spec.sort);
        json.key("filter");
        if (!spec.filter)
            json.null();
        else
        {
            json.beginObject()
                .key("combine")
                .value(spec.filter->combine == Filter::Combine::Or ? "or" : "and")
                .key("conditions")
                .beginArray();
            for (const FilterCondition &condition : spec.filter->conditions)
            {
                json.beginObject()
                    .key("column")
                    .value(condition.column)
                    .key("op")
                    .value(opName(condition.op))
                    .key("value")
                    .value(condition.value)
                    .endObject();
            }
            json.endArray().endObject();
        }
        json.endObject();
    }

    const char *columnKindName(ColumnKind kind)
    {
        switch (kind)
        {
        case ColumnKind::Unknown:
            return "unknown";
        case ColumnKind::Number:
            return "number";
        case ColumnKind::Date:
            return "date";
        case ColumnKind::DateTime:
            return "dateTime";
        case ColumnKind::Boolean:
            return "boolean";
        case ColumnKind::Text:
            return "text";
        case ColumnKind::Mixed:
            return "mixed";
        }
        return "unknown";
    }

    struct DateStyleName
    {
        DateStyle style;
        const char *name;
    };
    constexpr DateStyleName DATE_STYLE_NAMES[] = {
        {DateStyle::Iso, "iso"},
        {DateStyle::Slash, "slash"},
        {DateStyle::MonthDayYear, "monthDayYear"},
        {DateStyle::DayMonthYear, "dayMonthYear"},
        {DateStyle::Japanese, "japanese"},
        {DateStyle::MonthName, "monthName"},
    };

    const char *dateStyleName(DateStyle style)
    {
        for (const auto &entry : DATE_STYLE_NAMES)
        {
            if (entry.style == style)
                return entry.name;
        }
        return "iso";
    }

    DateStyle parseDateStyle(const std::string &name)
    {
        for (const auto &entry : DATE_STYLE_NAMES)
        {
            if (name == entry.name)
                return entry.style;
        }
        throw std::invalid_argument("unknown date style: " + name);
    }

    void writeFormat(JsonWriter &json, const ColumnFormat &format)
    {
        json.beginObject().key("thousands").value(format.thousands).key("decimals");
        if (format.decimals)
            json.value(*format.decimals);
        else
            json.null();
        json.key("date").value(dateStyleName(format.date)).endObject();
    }

    struct ExportFormatName
    {
        ViewExportFormat format;
        const char *name;
    };
    constexpr ExportFormatName EXPORT_FORMAT_NAMES[] = {
        {ViewExportFormat::Csv, "csv"},
        {ViewExportFormat::Tsv, "tsv"},
        {ViewExportFormat::Excel, "excel"},
        {ViewExportFormat::Parquet, "parquet"},
    };

    const char *exportFormatName(ViewExportFormat format)
    {
        for (const auto &entry : EXPORT_FORMAT_NAMES)
        {
            if (entry.format == format)
                return entry.name;
        }
        return "csv";
    }

    ViewExportFormat parseExportFormat(const std::string &name)
    {
        for (const auto &entry : EXPORT_FORMAT_NAMES)
        {
            if (name == entry.name)
                return entry.format;
        }
        throw std::invalid_argument("unknown export format: " + name);
    }

    Aggregate parseAggregate(const std::string &name)
    {
        for (const Aggregate aggregate : AGGREGATES)
        {
            if (name == aggregateName(aggregate))
                return aggregate;
        }
        throw std::invalid_argument("unknown aggregate: " + name);
    }

    HiddenColumns parseHidden(const JsonValue &value, size_t column_count)
    {
        HiddenColumns hidden;
        for (const size_t column : jsonIndexes(value, "hidden"))
        {
            if (column < column_count)
                hidden.hide(column, column, column_count);
        }
        return hidden;
    }

    void writeConditions(JsonWriter &json, const std::vector<FilterCondition> &conditions)
    {
        json.beginObject().key("conditions").beginArray();
        for (const FilterCondition &condition : conditions)
        {
            json.beginObject()
                .key("column")
                .value(condition.column)
                .key("op")
                .value(opName(condition.op))
                .key("value")
                .value(condition.value)
                .endObject();
        }
        json.endArray().endObject();
    }

    void writeTop(JsonWriter &json, const std::vector<ValueCount> &top)
    {
        json.beginArray();
        for (size_t i = 0; i < top.size() && i < TOP_VALUES; ++i)
            json.beginObject().key("value").value(cellDisplayText(top[i].value, 64)).key("count").value(top[i].count).endObject();
        json.endArray();
    }

    const char *histogramKindName(HistogramKind kind)
    {
        switch (kind)
        {
        case HistogramKind::Number:
            return "number";
        case HistogramKind::Time:
            return "time";
        case HistogramKind::None:
            break;
        }
        return "none";
    }

    void writeHistogram(JsonWriter &json, const Histogram &histogram)
    {
        json.beginObject()
            .key("kind")
            .value(histogramKindName(histogram.kind))
            .key("summary")
            .value(histogramSummary(histogram))
            .key("bins")
            .beginArray();
        for (size_t bin = 0; bin < histogram.binCount(); ++bin)
        {
            json.beginObject()
                .key("label")
                .value(histogramBinLabel(histogram, bin))
                .key("range")
                .value(histogramBinRange(histogram, bin))
                .key("count")
                .value(histogram.counts[bin])
                .endObject();
        }
        json.endArray().key("top");
        writeTop(json, histogram.top);
        json.endObject();
    }

    // An axis of a scatter plot: its range and ticks (the coarse layout's
    // edges) with their labels, and the bins of the density grid.
    void writeAxis(JsonWriter &json, const Histogram &axis, const Histogram &bins)
    {
        json.beginObject().key("kind").value(histogramKindName(axis.kind)).key("ticks").beginArray();
        for (const double edge : axis.edges)
            json.beginObject().key("at").value(edge).key("label").value(scatterValueText(axis, edge)).endObject();
        json.endArray().key("edges").beginArray();
        for (const double edge : bins.edges)
            json.value(edge);
        json.endArray().key("binRanges").beginArray();
        for (size_t bin = 0; bin < bins.binCount(); ++bin)
            json.value(histogramBinRange(bins, bin));
        json.endArray().endObject();
    }

    void writeBox(JsonWriter &json, const BoxStats &box)
    {
        json.beginObject()
            .key("count")
            .value(box.count)
            .key("min")
            .value(box.min)
            .key("q1")
            .value(box.q1)
            .key("median")
            .value(box.median)
            .key("q3")
            .value(box.q3)
            .key("max")
            .value(box.max)
            .key("lowWhisker")
            .value(box.low_whisker)
            .key("highWhisker")
            .value(box.high_whisker)
            .key("mean")
            .value(box.mean)
            .key("outliers")
            .beginArray();
        for (const double outlier : box.outliers)
            json.value(outlier);
        json.endArray().endObject();
    }

    // Waits on the calling thread for a task of the book's worker.
    template <typename T>
    class Waiter
    {
    public:
        void set(T value)
        {
            {
                std::lock_guard lock(mutex_);
                value_ = std::move(value);
            }
            cv_.notify_one();
        }

        T wait()
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]
                     { return value_.has_value(); });
            return std::move(*value_);
        }

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<T> value_;
    };

    const char *stateName(ModelState state)
    {
        switch (state)
        {
        case ModelState::Opening:
            return "opening";
        case ModelState::Ready:
            return "ready";
        case ModelState::Error:
            return "error";
        }
        return "error";
    }

    void writeInfo(JsonWriter &json, int id, const std::string &name, TableModel &model, const ColumnFormats &formats)
    {
        const ModelState state = model.state();
        const RowCount rows = model.rowCount();
        const ViewSpec view = model.view();
        json.beginObject()
            .key("sheet")
            .value(id)
            .key("name")
            .value(name)
            .key("state")
            .value(stateName(state));
        if (state == ModelState::Error)
            json.key("error").value(model.errorMessage());
        json.key("rows")
            .value(rows.value)
            .key("rowsExact")
            .value(rows.isExact())
            .key("displayRows")
            .value(model.displayRowCount())
            .key("rowsText")
            .value(filteredRowCountText(rows, model.unfilteredRowCount(), view.filtered()))
            .key("hasHeader")
            .value(model.hasHeader());

        const auto types = model.columnTypes();
        json.key("columns").beginArray();
        for (size_t column = 0; column < model.columnCount(); ++column)
        {
            const ColumnKind kind = column < types.size() ? types[column].kind : ColumnKind::Unknown;
            json.beginObject()
                .key("name")
                .value(model.columnName(column))
                .key("mark")
                .value(columnKindMark(kind))
                .key("kind")
                .value(columnKindName(kind))
                .key("format");
            if (const ColumnFormat *format = formats.find(column))
                writeFormat(json, *format);
            else
                json.null();
            json.endObject();
        }
        json.endArray();

        const ViewCapabilities capabilities = model.viewCapabilities();
        json.key("capabilities")
            .beginObject()
            .key("sort")
            .value(capabilities.sort)
            .key("filter")
            .value(capabilities.filter)
            .key("search")
            .value(capabilities.search)
            .endObject();

        json.key("view");
        writeView(json, view);
        json.key("pending").value(model.requestedView() != view);
        json.key("filterText").value(view.filter ? filterSummary(model, *view.filter) : "");
        if (const auto error = model.takeViewError())
            json.key("viewError").value(*error);

        json.key("checkpoint");
        if (const auto checkpoint = model.memoryCheckpoint())
            json.beginObject().key("bytes").value(checkpoint->bytes).key("rows").value(checkpoint->rows).endObject();
        else
            json.null();
        json.key("readingStopped").value(model.readingStopped());
        json.endObject();
    }

    // One character per cell, for the frontend to style it by.
    char cellKind(const CellRef &ref)
    {
        switch (ref.state)
        {
        case CellState::Ready:
            if (isEmptyCell(*ref.value))
                return 'e';
            return isNumericCell(*ref.value) ? 'n' : 's';
        case CellState::Loading:
            return 'l';
        case CellState::Empty:
            return 'e';
        case CellState::OutOfRange:
            return 'o';
        case CellState::Error:
            return 'x';
        }
        return 'x';
    }

    void writeRows(JsonWriter &json, TableModel &model, const ColumnFormats &formats, size_t first, size_t last,
                   size_t first_column, size_t last_column)
    {
        model.requestRange(first, last);
        const size_t columns = model.columnCount();
        json.beginObject().key("first").value(first).key("firstColumn").value(first_column).key("rows").beginArray();
        std::vector<std::string> kinds;
        // A sheet being opened has no columns yet; its rows come later.
        bool loading = model.state() == ModelState::Opening;
        bool end = columns == 0 && !loading;
        for (size_t row = first; columns > 0 && row <= last; ++row)
        {
            const CellRef probe = model.peek(row, 0);
            if (probe.state == CellState::OutOfRange || probe.state == CellState::Error)
            {
                end = true;
                break;
            }
            std::string row_kinds;
            json.beginArray();
            for (size_t column = first_column; column <= last_column && column < columns; ++column)
            {
                const CellRef ref = column == 0 ? probe : model.peek(row, column);
                const char kind = cellKind(ref);
                row_kinds += kind;
                loading = loading || kind == 'l';
                json.value(ref.state == CellState::Ready ? cellDisplayText(*ref.value, MAX_CELL_BYTES, formats.find(column))
                                                         : std::string());
            }
            json.endArray();
            kinds.push_back(std::move(row_kinds));
        }
        json.endArray().key("kinds").beginArray();
        for (const std::string &row_kinds : kinds)
            json.value(row_kinds);
        json.endArray().key("loading").value(loading).key("end").value(end).endObject();
    }

    void writeSearchState(JsonWriter &json, const SearchState &state)
    {
        json.beginObject()
            .key("active")
            .value(state.active)
            .key("done")
            .value(state.done)
            .key("truncated")
            .value(state.truncated)
            .key("error")
            .value(state.error)
            .key("scannedRows")
            .value(state.scanned_rows)
            .key("hits")
            .value(state.hits.size())
            .key("generation")
            .value(static_cast<double>(state.generation))
            .endObject();
    }

}

// Request parameters, with errors that name the parameter.
class WebSession::Params
{
public:
    explicit Params(const JsonValue &value) : value_(value) {}

    const JsonValue &operator[](std::string_view key) const { return value_[key]; }

    size_t index(std::string_view key) const
    {
        return jsonIndex(value_[key], key);
    }

    size_t index(std::string_view key, size_t fallback) const
    {
        return value_[key].isNull() ? fallback : index(key);
    }

    int integer(std::string_view key) const
    {
        const auto number = value_[key].number();
        if (!number)
            throw std::invalid_argument("expected a number: " + std::string(key));
        return static_cast<int>(*number);
    }

    const std::string &text(std::string_view key) const
    {
        const std::string *text = value_[key].string();
        if (!text)
            throw std::invalid_argument("expected a string: " + std::string(key));
        return *text;
    }

    double number(std::string_view key) const
    {
        const auto number = value_[key].number();
        if (!number)
            throw std::invalid_argument("expected a number: " + std::string(key));
        return *number;
    }

    bool flag(std::string_view key) const
    {
        return value_[key].boolean().value_or(false);
    }

private:
    const JsonValue &value_;
};


WebSession::WebSession(EventSink sink, std::filesystem::path scratch)
    : sink_(std::move(sink)),
      scratch_(scratch.empty() ? std::filesystem::temp_directory_path() / "tableskim-web" : std::move(scratch))
{
}

WebSession::~WebSession()
{
    close();
}

const std::map<std::string_view, WebSession::Method> &WebSession::methods()
{
    static const std::map<std::string_view, Method> table = {
        {"open", &WebSession::open},
        {"close", &WebSession::closeBook},
        {"sheet", &WebSession::openSheet},
        {"info", &WebSession::info},
        {"rows", &WebSession::rows},
        {"setView", &WebSession::setView},
        {"sortCycle", &WebSession::sortCycle},
        {"search", &WebSession::search},
        {"cancelSearch", &WebSession::cancelSearch},
        {"searchState", &WebSession::searchState},
        {"findHit", &WebSession::findHit},
        {"continueReading", &WebSession::continueReading},
        {"stopReading", &WebSession::stopReading},
        {"copy", &WebSession::copy},
        {"goTo", &WebSession::goTo},
        {"filterOps", &WebSession::filterOps},
        {"cellFilter", &WebSession::cellFilter},
        {"setFormat", &WebSession::setFormat},
        {"dateStyles", &WebSession::dateStyles},
        {"sql", &WebSession::sql},
        {"sqlSchema", &WebSession::sqlSchema},
        {"completeSql", &WebSession::completeSql},
        {"startStats", &WebSession::startStats},
        {"statsState", &WebSession::statsState},
        {"startHistogram", &WebSession::startHistogram},
        {"histogramState", &WebSession::histogramState},
        {"histogramFilter", &WebSession::histogramFilter},
        {"defaultScatter", &WebSession::defaultScatter},
        {"startScatter", &WebSession::startScatter},
        {"scatterState", &WebSession::scatterState},
        {"scatterFilter", &WebSession::scatterFilter},
        {"exportFormats", &WebSession::exportFormats},
        {"startExport", &WebSession::startExport},
        {"aggregates", &WebSession::aggregates},
        {"startGroupBy", &WebSession::startGroupBy},
        {"jobState", &WebSession::jobState},
        {"cancelJob", &WebSession::cancelJob},
    };
    return table;
}

void WebSession::close()
{
    // The job reads the book's rows: it goes first.
    stopJob();
    sheets_.clear();
    book_.reset();
    schema_.reset();
    path_.clear();
}

void WebSession::stopJob()
{
    if (!job_)
        return;
    job_->thread.request_stop();
    if (job_->thread.joinable())
        job_->thread.join();
    std::error_code ec;
    if (!job_->file.empty())
        std::filesystem::remove(job_->file, ec);
    job_.reset();
}

WebSession::Sheet &WebSession::sheet(int id)
{
    for (const auto &sheet : sheets_)
    {
        if (sheet->id == id)
            return *sheet;
    }
    throw std::invalid_argument("no such sheet: " + std::to_string(id));
}

WebSession::Sheet &WebSession::sheet(const Params &params)
{
    return sheet(params.integer("sheet"));
}

void WebSession::dropSheets(const std::string &name)
{
    std::erase_if(sheets_, [&](const auto &sheet)
                  { return sheet->name == name; });
}

void WebSession::writeSheetNames(JsonWriter &json) const
{
    json.beginArray();
    if (book_)
    {
        for (const std::string &name : book_->sheetNames())
            json.value(name);
    }
    json.endArray();
}

std::string WebSession::handle(std::string_view request_text)
{
    JsonWriter reply;
    JsonValue request;
    try
    {
        request = JsonValue::parse(request_text);
    }
    catch (const std::exception &e)
    {
        reply.beginObject().key("id").null().key("error").value(e.what()).endObject();
        return reply.take();
    }

    reply.beginObject().key("id");
    if (const auto id = request["id"].number())
        reply.value(*id);
    else
        reply.null();

    // The result is written aside, so that a failure halfway leaves no half
    // of it in the reply.
    JsonWriter result;
    try
    {
        const std::string *method = request["method"].string();
        if (!method)
            throw std::invalid_argument("no method");
        const auto found = methods().find(*method);
        if (found == methods().end())
            throw std::invalid_argument("unknown method: " + *method);
        (this->*found->second)(Params(request["params"]), result);
    }
    catch (const std::exception &e)
    {
        reply.key("error").value(e.what()).endObject();
        return reply.take();
    }
    reply.key("result").raw(result.text()).endObject();
    return reply.take();
}

void WebSession::open(const Params &params, JsonWriter &result)
{
    close();
    OpenOptions options;
    if (const std::string *header = params["header"].string())
    {
        if (*header == "yes")
            options.header = OpenOptions::Header::Yes;
        else if (*header == "no")
            options.header = OpenOptions::Header::No;
    }
    if (const std::string *encoding = params["encoding"].string(); encoding && !encoding->empty())
    {
        options.encoding = parseTextEncoding(*encoding);
        if (!options.encoding)
            throw std::invalid_argument("unknown encoding: " + *encoding);
    }
    path_ = utf8Path(params.text("path"));
    book_ = TableBook::open(path_, options);
    result.beginObject().key("sheets");
    writeSheetNames(result);
    result.key("usesHeaderOption")
        .value(book_->usesHeaderOption())
        .key("isTextFormat")
        .value(book_->isTextFormat())
        .key("supportsSql")
        .value(book_->supportsSql())
        .key("textEncoding");
    if (const auto encoding = book_->textEncoding())
        result.value(textEncodingName(*encoding));
    else
        result.null();
    result.endObject();
}

void WebSession::closeBook(const Params &, JsonWriter &result)
{
    close();
    result.beginObject().endObject();
}

void WebSession::openSheet(const Params &params, JsonWriter &result)
{
    if (!book_)
        throw std::invalid_argument("no file is open");
    const std::string &name = params.text("name");
    Sheet *found = nullptr;
    for (const auto &sheet : sheets_)
    {
        if (sheet->name == name)
            found = sheet.get();
    }
    if (!found)
    {
        auto model = book_->sheet(name);
        if (!model)
            throw std::invalid_argument("no such sheet: " + name);
        auto sheet = std::make_unique<Sheet>();
        sheet->id = next_sheet_id_++;
        sheet->name = name;
        sheet->model = std::move(model);
        sheet->subscription = sheet->model->subscribe([sink = sink_, id = sheet->id](const TableEvent &event)
                                                      {
            if (const unsigned events = eventBits(event.kind))
                sink(id, events); });
        found = sheet.get();
        sheets_.push_back(std::move(sheet));
    }
    writeInfo(result, found->id, found->name, *found->model, found->formats);
}

void WebSession::info(const Params &params, JsonWriter &result)
{
    Sheet &sheet = this->sheet(params);
    writeInfo(result, sheet.id, sheet.name, *sheet.model, sheet.formats);
}

void WebSession::rows(const Params &params, JsonWriter &result)
{
    Sheet &sheet = this->sheet(params);
    const size_t first = params.index("first");
    const size_t last = std::max(first, std::min(params.index("last"), first + MAX_ROWS - 1));
    const size_t first_column = params.index("firstColumn", 0);
    const size_t last_column = std::max(
        first_column, std::min(params.index("lastColumn", first_column + MAX_COLUMNS - 1), first_column + MAX_COLUMNS - 1));
    writeRows(result, *sheet.model, sheet.formats, first, last, first_column, last_column);
}

void WebSession::setView(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    ViewSpec spec = model.requestedView();
    spec.sort = parseSort(params["sort"]);
    spec.filter = parseFilter(params["filter"]);
    if (spec.filter)
    {
        if (const auto error = filterError(*spec.filter))
            throw std::invalid_argument(*error);
    }
    model.setView(spec);
    result.beginObject().key("view");
    writeView(result, spec);
    result.endObject();
}

void WebSession::sortCycle(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    ViewSpec spec = model.requestedView();
    spec.sort = nextSortCycle(spec.sort, params.index("column"));
    model.setView(spec);
    result.beginObject().key("view");
    writeView(result, spec);
    result.endObject();
}

void WebSession::search(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    SearchSpec spec;
    spec.text = params.text("text");
    spec.case_sensitive = params.flag("caseSensitive");
    spec.regex = params.flag("regex");
    if (!params["column"].isNull())
        spec.column = params.index("column");
    model.startSearch(std::move(spec));
    writeSearchState(result, model.searchState());
}

void WebSession::cancelSearch(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    model.cancelSearch();
    writeSearchState(result, model.searchState());
}

void WebSession::searchState(const Params &params, JsonWriter &result)
{
    writeSearchState(result, sheet(params).model->searchState());
}

void WebSession::findHit(const Params &params, JsonWriter &result)
{
    const SearchState state = sheet(params).model->searchState();
    const CellPosition from{params.index("row"), params.index("column")};
    const SearchHitLookup lookup =
        findSearchHit(state, from, params["forward"].boolean().value_or(true), params.flag("includeFrom"));
    result.beginObject().key("index");
    if (lookup.index)
    {
        const CellPosition hit = state.hits[*lookup.index];
        result.value(*lookup.index).key("row").value(hit.row).key("column").value(hit.col);
    }
    else
        result.null();
    result.key("hits").value(state.hits.size()).key("wrapped").value(lookup.wrapped).key("pending").value(lookup.pending).endObject();
}

void WebSession::continueReading(const Params &params, JsonWriter &result)
{
    sheet(params).model->continueReading();
    result.beginObject().endObject();
}

void WebSession::stopReading(const Params &params, JsonWriter &result)
{
    sheet(params).model->stopReading();
    result.beginObject().endObject();
}

void WebSession::copy(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    const HiddenColumns hidden = parseHidden(params["hidden"], model.columnCount());
    const TsvCopyResult copied =
        rangeToTsv(model, params.index("top"), params.index("left"), params.index("bottom"), params.index("right"), &hidden);
    result.beginObject()
        .key("text")
        .value(copied.text)
        .key("copiedRows")
        .value(copied.copied_rows)
        .key("skippedRows")
        .value(copied.skipped_rows)
        .endObject();
}

void WebSession::goTo(const Params &params, JsonWriter &result)
{
    const GoToResult target = parseGoTo(params.text("text"), *sheet(params).model);
    result.beginObject();
    if (!target.target)
        result.key("error").value(target.error);
    else
    {
        result.key("row");
        if (target.target->row)
            result.value(*target.target->row);
        else
            result.null();
        result.key("column");
        if (target.target->column)
            result.value(*target.target->column);
        else
            result.null();
    }
    result.endObject();
}

void WebSession::filterOps(const Params &, JsonWriter &result)
{
    result.beginArray();
    for (const FilterOp op : FILTER_OPS)
    {
        result.beginObject()
            .key("op")
            .value(opName(op))
            .key("label")
            .value(filterOpLabel(op))
            .key("takesValue")
            .value(filterOpTakesValue(op))
            .endObject();
    }
    result.endArray();
}

void WebSession::cellFilter(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    const size_t column = params.index("column");
    const CellRef ref = model.peek(params.index("row"), column);
    if (ref.state == CellState::Loading || ref.state == CellState::OutOfRange || ref.state == CellState::Error)
        throw std::invalid_argument("the cell is not loaded");
    // A ragged row's missing cell filters as an empty one.
    const CellValue value = ref.state == CellState::Ready ? *ref.value : CellValue{};
    writeConditions(result, {cellFilterCondition(column, value, params["equal"].boolean().value_or(true))});
}

void WebSession::setFormat(const Params &params, JsonWriter &result)
{
    Sheet &sheet = this->sheet(params);
    ColumnFormat format;
    format.thousands = params.flag("thousands");
    if (!params["decimals"].isNull())
        format.decimals = static_cast<int>(std::min<size_t>(params.index("decimals"), MAX_DECIMALS));
    if (const std::string *date = params["date"].string())
        format.date = parseDateStyle(*date);
    sheet.formats.set(params.index("column"), format);
    result.beginObject().endObject();
}

void WebSession::dateStyles(const Params &, JsonWriter &result)
{
    result.beginArray();
    for (const DateStyle style : DATE_STYLES)
        result.beginObject().key("style").value(dateStyleName(style)).key("example").value(dateStyleExample(style)).endObject();
    result.endArray();
}

void WebSession::sql(const Params &params, JsonWriter &result)
{
    if (!book_ || !book_->supportsSql())
        throw std::invalid_argument("this file has no SQL");
    // The query runs on the book's worker, which reads the rows too: waiting
    // here keeps nothing from loading that could load meanwhile.
    Waiter<TableBook::SqlResult> waiter;
    book_->executeSql(params.text("sql"), [&](const TableBook::SqlResult &outcome)
                      { waiter.set(outcome); });
    const TableBook::SqlResult outcome = waiter.wait();
    schema_.reset(); // a query may create or drop temporary objects
    if (!outcome.ok)
        throw std::invalid_argument(outcome.message);
    dropSheets(outcome.sheet);
    result.beginObject().key("sheet").value(outcome.sheet).key("sheets");
    writeSheetNames(result);
    result.endObject();
}

void WebSession::sqlSchema(const Params &, JsonWriter &result)
{
    if (!book_ || !book_->supportsSql())
        throw std::invalid_argument("this file has no SQL");
    if (!schema_)
    {
        Waiter<std::vector<SqlTable>> waiter;
        book_->loadSqlSchema([&](std::vector<SqlTable> tables)
                             { waiter.set(std::move(tables)); });
        schema_ = waiter.wait();
    }
    result.beginArray();
    for (const SqlTable &table : *schema_)
    {
        result.beginObject().key("name").value(table.name).key("columns").beginArray();
        for (const std::string &column : table.columns)
            result.value(column);
        result.endArray().endObject();
    }
    result.endArray();
}

void WebSession::completeSql(const Params &params, JsonWriter &result)
{
    if (!schema_)
    {
        JsonWriter ignored;
        sqlSchema(params, ignored);
    }
    const std::string &text = params.text("sql");
    const size_t cursor = std::min(params.index("cursor"), text.size());
    const SqlCompletion completion = ::completeSql(text, cursor, *schema_);
    result.beginObject()
        .key("begin")
        .value(completion.begin)
        .key("end")
        .value(completion.end)
        .key("common")
        .value(commonCompletionPrefix(completion.candidates))
        .key("candidates")
        .beginArray();
    for (const std::string &candidate : completion.candidates)
        result.value(candidate);
    result.endArray().endObject();
}

void WebSession::startStats(const Params &params, JsonWriter &result)
{
    sheet(params).model->startColumnStats(params.index("column"));
    statsState(params, result);
}

void WebSession::statsState(const Params &params, JsonWriter &result)
{
    const ColumnStatsState state = sheet(params).model->columnStatsState();
    result.beginObject()
        .key("active")
        .value(state.active)
        .key("done")
        .value(state.done)
        .key("column")
        .value(state.column)
        .key("scannedRows")
        .value(state.scanned_rows)
        .key("lines")
        .beginArray();
    for (const StatsLine &line : columnStatsLines(state.stats))
        result.beginObject().key("label").value(line.label).key("value").value(line.value).endObject();
    result.endArray().key("top");
    writeTop(result, state.stats.top);
    result.endObject();
}

void WebSession::startHistogram(const Params &params, JsonWriter &result)
{
    sheet(params).model->startHistogram(params.index("column"), std::clamp<size_t>(params.index("bins", 30), 2, 200));
    histogramState(params, result);
}

void WebSession::histogramState(const Params &params, JsonWriter &result)
{
    const HistogramState state = sheet(params).model->histogramState();
    result.beginObject()
        .key("active")
        .value(state.active)
        .key("done")
        .value(state.done)
        .key("counting")
        .value(state.counting)
        .key("column")
        .value(state.column)
        .key("scannedRows")
        .value(state.scanned_rows)
        .key("histogram");
    writeHistogram(result, state.histogram);
    result.endObject();
}

void WebSession::histogramFilter(const Params &params, JsonWriter &result)
{
    const HistogramState state = sheet(params).model->histogramState();
    const Histogram &histogram = state.histogram;
    const size_t bin = params.index("bin");
    if (!state.done || bin >= (histogram.binCount() > 0 ? histogram.binCount() : histogram.top.size()))
        throw std::invalid_argument("no such bin");
    // Without bins, the histogram shows the most frequent values instead.
    if (histogram.binCount() > 0)
        writeConditions(result, histogramBinConditions(histogram, state.column, bin));
    else
        writeConditions(result, {cellFilterCondition(state.column, histogram.top[bin].value, true)});
}

void WebSession::defaultScatter(const Params &params, JsonWriter &result)
{
    TableModel &model = *sheet(params).model;
    const HiddenColumns hidden = parseHidden(params["hidden"], model.columnCount());
    const size_t first_row = params.index("firstRow", 0);
    const ScatterColumns columns = defaultScatterColumns(model, params.index("column"), hidden, first_row,
                                                         std::max(first_row + 1, params.index("lastRow", first_row + 100)));
    result.beginObject().key("x").value(columns.x).key("y").value(columns.y).endObject();
}

void WebSession::startScatter(const Params &params, JsonWriter &result)
{
    ScatterOptions options;
    options.max_points = params["points"].boolean().value_or(true) ? SCATTER_POINTS : 0;
    sheet(params).model->startScatter(params.index("x"), params.index("y"), options);
    result.beginObject().endObject();
}

void WebSession::scatterState(const Params &params, JsonWriter &result)
{
    const ScatterState state = sheet(params).model->scatterState();
    const ScatterPlot &plot = state.plot;
    result.beginObject()
        .key("active")
        .value(state.active)
        .key("done")
        .value(state.done)
        .key("counting")
        .value(state.counting)
        .key("x")
        .value(state.x_column)
        .key("y")
        .value(state.y_column)
        .key("scannedRows")
        .value(state.scanned_rows)
        .key("plottable")
        .value(plot.plottable())
        .key("summary")
        .value(scatterSummary(plot, state.options.max_points > 0))
        .key("statsText")
        .value(scatterStatsText(plot))
        .key("pairs")
        .value(plot.pairs)
        .key("xAxis");
    writeAxis(result, plot.x_axis, plot.x_bins);
    result.key("yAxis");
    writeAxis(result, plot.y_axis, plot.y_bins);
    // The density grid, by X bin, then Y bin (ScatterPlot::densityIndex()).
    result.key("counts").beginArray();
    for (const size_t count : plot.counts)
        result.value(count);
    result.endArray().key("categories").beginArray();
    for (size_t category = 0; category < plot.xBinCount() && plot.categorical(); ++category)
        result.value(scatterCategoryText(plot, category));
    result.endArray().key("boxes").beginArray();
    for (size_t category = 0; category < plot.boxes.size(); ++category)
    {
        writeBox(result, plot.boxes[category]);
    }
    result.endArray().key("boxTexts").beginArray();
    for (size_t category = 0; category < plot.boxes.size(); ++category)
        result.value(boxPlotText(plot, category));
    // Points as x, y, view row, ... (only when asked for: they are many).
    result.endArray().key("points").beginArray();
    if (params.flag("points") && plot.points)
    {
        for (const ScatterPoint &point : *plot.points)
            result.value(point.x).value(point.y).value(point.view_row);
    }
    result.endArray().endObject();
}

void WebSession::scatterFilter(const Params &params, JsonWriter &result)
{
    const ScatterState state = sheet(params).model->scatterState();
    if (!state.done || !state.plot.plottable())
        throw std::invalid_argument("the plot is not ready");
    const ScatterPlot &plot = state.plot;
    if (!params["category"].isNull())
    {
        const size_t category = params.index("category");
        if (!plot.categorical() || category >= plot.xBinCount())
            throw std::invalid_argument("no such category");
        writeConditions(result, scatterCategoryConditions(plot, state.x_column, state.y_column, category));
        return;
    }
    if (!params["xLow"].isNull())
    {
        // A rectangle drawn over the plot.
        if (plot.categorical())
            throw std::invalid_argument("categories have no range");
        writeConditions(result, scatterRangeConditions(plot, state.x_column, state.y_column, params.number("xLow"),
                                                       params.number("xHigh"), params.number("yLow"),
                                                       params.number("yHigh")));
        return;
    }
    const size_t x_bin = params.index("xBin");
    const size_t y_bin = params.index("yBin");
    if (x_bin >= plot.xBinCount() || y_bin >= plot.y_bins.binCount())
        throw std::invalid_argument("no such cell");
    writeConditions(result, densityCellConditions(plot, state.x_column, state.y_column, x_bin, y_bin));
}

void WebSession::exportFormats(const Params &, JsonWriter &result)
{
    result.beginArray();
    for (const ViewExportFormat format : VIEW_EXPORT_FORMATS)
    {
        result.beginObject()
            .key("format")
            .value(exportFormatName(format))
            .key("label")
            .value(viewExportFormatName(format))
            .key("extension")
            .value(viewExportFormatExtension(format))
            .endObject();
    }
    result.endArray();
}

void WebSession::startExport(const Params &params, JsonWriter &result)
{
    if (job_ && !job_->done)
        throw std::invalid_argument("another export or Group By is running");
    stopJob();
    Sheet &sheet = this->sheet(params);
    const ViewExportFormat format = parseExportFormat(params.text("format"));
    ViewExportOptions options;
    options.hidden = parseHidden(params["hidden"], sheet.model->columnCount());
    options.source = path_;
    std::filesystem::create_directories(scratch_);

    job_ = std::make_unique<Job>();
    Job &job = *job_;
    job.kind = "export";
    job.file = scratch_ / ("export-" + std::to_string(next_export_++) + viewExportFormatExtension(format));
    // Sheets of a file with one sheet are named after the file.
    const bool several = book_->sheetNames().size() > 1;
    job.file_name = utf8String(defaultViewExportPath(path_, several ? sheet.name : "", format).filename());
    job.thread = std::jthread([this, &job, model = sheet.model, format, options](std::stop_token stop)
                              {
        std::string error;
        try
        {
            if (!exportView(model, job.file, format, options, stop, [&](size_t rows)
                            {
                                job.rows = rows;
                                sink_(SESSION, JOB);
                            }))
                error = "stopped";
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        {
            std::lock_guard lock(job.mutex);
            job.error = error;
        }
        job.done = true;
        sink_(SESSION, JOB); });
    result.beginObject().endObject();
}

void WebSession::aggregates(const Params &, JsonWriter &result)
{
    result.beginArray();
    for (const Aggregate aggregate : AGGREGATES)
        result.value(aggregateName(aggregate));
    result.endArray();
}

void WebSession::startGroupBy(const Params &params, JsonWriter &result)
{
    if (job_ && !job_->done)
        throw std::invalid_argument("another export or Group By is running");
    stopJob();
    Sheet &sheet = this->sheet(params);
    GroupBySpec spec;
    spec.keys = jsonIndexes(params["keys"], "keys");
    spec.values = jsonIndexes(params["values"], "values");
    if (const auto *names = params["aggregates"].array())
    {
        for (const JsonValue &name : *names)
        {
            if (const std::string *text = name.string())
                spec.aggregates.push_back(parseAggregate(*text));
        }
    }
    for (const size_t column : spec.keys)
    {
        if (column >= sheet.model->columnCount())
            throw std::invalid_argument("no such column: " + std::to_string(column));
    }
    for (const size_t column : spec.values)
    {
        if (column >= sheet.model->columnCount())
            throw std::invalid_argument("no such column: " + std::to_string(column));
    }

    job_ = std::make_unique<Job>();
    Job &job = *job_;
    job.kind = "groupBy";
    job.thread = std::jthread([this, &job, model = sheet.model, spec](std::stop_token stop)
                              {
        std::string error;
        std::optional<ResultSheet> rows;
        try
        {
            rows = groupByView(model, spec, stop, [&](size_t count)
                               {
                                   job.rows = count;
                                   sink_(SESSION, JOB);
                               });
            if (!rows)
                error = "stopped";
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        {
            std::lock_guard lock(job.mutex);
            job.error = error;
            job.group_rows = std::move(rows);
        }
        job.done = true;
        sink_(SESSION, JOB); });
    result.beginObject().endObject();
}

void WebSession::jobState(const Params &, JsonWriter &result)
{
    result.beginObject();
    if (!job_)
    {
        result.key("kind").null().endObject();
        return;
    }
    Job &job = *job_;
    const bool done = job.done;
    std::lock_guard lock(job.mutex);
    // The result sheet is added here, on the session's thread, which owns
    // the sheets.
    if (done && job.group_rows && book_)
    {
        job.sheet = book_->addResultSheet(GROUP_BY_SHEET, ResultRows::make(std::move(*job.group_rows)));
        job.group_rows.reset();
        dropSheets(job.sheet);
    }
    result.key("kind").value(job.kind).key("done").value(done).key("rows").value(job.rows.load());
    if (done)
    {
        result.key("error").value(job.error);
        if (job.error.empty() && job.kind == "export")
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(job.file, ec);
            result.key("file")
                .value(job.file.generic_string())
                .key("fileName")
                .value(job.file_name)
                .key("size")
                .value(static_cast<double>(ec ? 0 : size));
        }
        if (!job.sheet.empty())
        {
            result.key("sheet").value(job.sheet).key("sheets");
            writeSheetNames(result);
        }
    }
    result.endObject();
}

void WebSession::cancelJob(const Params &, JsonWriter &result)
{
    stopJob();
    result.beginObject().endObject();
}
