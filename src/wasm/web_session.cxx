#include "web_session.hxx"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "cell_format.hxx"
#include "column_type.hxx"
#include "file_util.hxx"
#include "search_hits.hxx"
#include "text_encoding.hxx"
#include "web_json.hxx"

namespace
{
    // The most rows and columns one "rows" request returns.
    constexpr size_t MAX_ROWS = 1000;
    constexpr size_t MAX_COLUMNS = 500;
    // Cell text is cut there, as the GUI does when drawing.
    constexpr size_t MAX_CELL_BYTES = 256;

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
        case TableEvent::Kind::HistogramProgress:
        case TableEvent::Kind::ScatterProgress:
            break;
        }
        return 0;
    }

    // Request parameters, with errors that name the parameter.
    class Params
    {
    public:
        explicit Params(const JsonValue &value) : value_(value) {}

        const JsonValue &operator[](std::string_view key) const { return value_[key]; }

        size_t index(std::string_view key) const
        {
            const auto number = value_[key].number();
            if (!number || *number < 0 || *number != std::floor(*number))
                throw std::invalid_argument("expected a non-negative integer: " + std::string(key));
            return static_cast<size_t>(std::min(*number, 9.0e15));
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

        bool flag(std::string_view key) const
        {
            return value_[key].boolean().value_or(false);
        }

    private:
        const JsonValue &value_;
    };

    std::vector<SortKey> parseSort(const JsonValue &value)
    {
        std::vector<SortKey> sort;
        if (const auto *keys = value.array())
        {
            for (const JsonValue &key : *keys)
            {
                const Params params(key);
                sort.push_back({params.index("column"), key["ascending"].boolean().value_or(true)});
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
                const Params params(condition);
                const std::string *text = condition["value"].string();
                filter.conditions.push_back({params.index("column"), parseOp(params.text("op")), text ? *text : ""});
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

    void writeInfo(JsonWriter &json, int id, const std::string &name, TableModel &model)
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
            json.beginObject().key("name").value(model.columnName(column)).key("mark").value(columnKindMark(kind)).endObject();
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

    void writeRows(JsonWriter &json, TableModel &model, size_t first, size_t last, size_t first_column, size_t last_column)
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
                json.value(ref.state == CellState::Ready ? cellDisplayText(*ref.value, MAX_CELL_BYTES) : std::string());
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

WebSession::WebSession(EventSink sink) : sink_(std::move(sink))
{
}

WebSession::~WebSession()
{
    close();
}

void WebSession::close()
{
    sheets_.clear();
    book_.reset();
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
        const std::string *method_name = request["method"].string();
        if (!method_name)
            throw std::invalid_argument("no method");
        const std::string &method = *method_name;
        const Params params(request["params"]);

        if (method == "open")
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
            book_ = TableBook::open(utf8Path(params.text("path")), options);
            result.beginObject().key("sheets").beginArray();
            for (const std::string &name : book_->sheetNames())
                result.value(name);
            result.endArray()
                .key("usesHeaderOption")
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
        else if (method == "close")
        {
            close();
            result.beginObject().endObject();
        }
        else if (method == "sheet")
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
            writeInfo(result, found->id, found->name, *found->model);
        }
        else if (method == "info")
        {
            Sheet &sheet = this->sheet(params.integer("sheet"));
            writeInfo(result, sheet.id, sheet.name, *sheet.model);
        }
        else if (method == "rows")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            const size_t first = params.index("first");
            const size_t last = std::max(first, std::min(params.index("last"), first + MAX_ROWS - 1));
            const size_t first_column = params.index("firstColumn", 0);
            const size_t last_column =
                std::max(first_column, std::min(params.index("lastColumn", first_column + MAX_COLUMNS - 1), first_column + MAX_COLUMNS - 1));
            writeRows(result, model, first, last, first_column, last_column);
        }
        else if (method == "setView" || method == "sortCycle")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            ViewSpec spec = model.requestedView();
            if (method == "sortCycle")
                spec.sort = nextSortCycle(spec.sort, params.index("column"));
            else
            {
                spec.sort = parseSort(params["sort"]);
                spec.filter = parseFilter(params["filter"]);
                if (spec.filter)
                {
                    if (const auto error = filterError(*spec.filter))
                        throw std::invalid_argument(*error);
                }
            }
            model.setView(spec);
            result.beginObject().key("view");
            writeView(result, spec);
            result.endObject();
        }
        else if (method == "search")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            SearchSpec spec;
            spec.text = params.text("text");
            spec.case_sensitive = params.flag("caseSensitive");
            spec.regex = params.flag("regex");
            if (!params["column"].isNull())
                spec.column = params.index("column");
            model.startSearch(std::move(spec));
            writeSearchState(result, model.searchState());
        }
        else if (method == "cancelSearch")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            model.cancelSearch();
            writeSearchState(result, model.searchState());
        }
        else if (method == "searchState")
        {
            writeSearchState(result, sheet(params.integer("sheet")).model->searchState());
        }
        else if (method == "findHit")
        {
            const SearchState state = sheet(params.integer("sheet")).model->searchState();
            const CellPosition from{params.index("row"), params.index("column")};
            const SearchHitLookup lookup = findSearchHit(state, from, params["forward"].boolean().value_or(true),
                                                         params.flag("includeFrom"));
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
        else if (method == "continueReading" || method == "stopReading")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            if (method == "continueReading")
                model.continueReading();
            else
                model.stopReading();
            result.beginObject().endObject();
        }
        else if (method == "copy")
        {
            TableModel &model = *sheet(params.integer("sheet")).model;
            const TsvCopyResult copied =
                rangeToTsv(model, params.index("top"), params.index("left"), params.index("bottom"), params.index("right"));
            result.beginObject()
                .key("text")
                .value(copied.text)
                .key("copiedRows")
                .value(copied.copied_rows)
                .key("skippedRows")
                .value(copied.skipped_rows)
                .endObject();
        }
        else if (method == "filterOps")
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
        else
            throw std::invalid_argument("unknown method: " + method);
    }
    catch (const std::exception &e)
    {
        reply.key("error").value(e.what()).endObject();
        return reply.take();
    }
    reply.key("result").raw(result.text()).endObject();
    return reply.take();
}
