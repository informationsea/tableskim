#ifndef _WEB_SESSION_HXX_
#define _WEB_SESSION_HXX_

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cell_format.hxx"
#include "sql_text.hxx"
#include "table.hxx"

class JsonWriter;

// The web frontend's view of the core: one book at a time and the sheets
// shown from it, driven by JSON requests
//   {"id": 1, "method": "rows", "params": {...}}
// answered by {"id": 1, "result": {...}} or {"id": 1, "error": "..."}.
// See src/wasm/README.md for the methods. handle() is called on one thread
// (the session thread of the WebAssembly module); it does not wait for the
// book's worker to read rows, so a reply may say that rows are still
// loading: the sink then reports the change and the frontend asks again.
// Exports and Group By run as a job on a thread of their own, which the
// frontend follows with "jobState".
class WebSession
{
public:
    // What changed in a sheet, as a bit set.
    enum Event : unsigned
    {
        ROWS = 1 << 0,       // rows loaded or cells edited
        ROW_COUNT = 1 << 1,  // the row count, or the columns
        VIEW = 1 << 2,       // a sort or filter was applied (or failed)
        SEARCH = 1 << 3,     // search progress
        CHECKPOINT = 1 << 4, // reading paused at a memory checkpoint
        STATE = 1 << 5,      // opened, or failed
        STATS = 1 << 6,      // column statistics progress
        HISTOGRAM = 1 << 7,  // histogram progress
        SCATTER = 1 << 8,    // scatter plot progress
        JOB = 1 << 9,        // the job's progress, reported for SESSION
    };
    // The "sheet" that session-wide events (JOB) are reported for.
    static constexpr int SESSION = 0;
    // Called on the book's worker thread and the job's, once per event:
    // coalesce them.
    using EventSink = std::function<void(int sheet, unsigned events)>;

    // Exports are written into `scratch` (the default: a directory in the
    // temporary directory), where the frontend reads them from.
    explicit WebSession(EventSink sink, std::filesystem::path scratch = {});
    ~WebSession();
    WebSession(const WebSession &) = delete;
    WebSession &operator=(const WebSession &) = delete;

    std::string handle(std::string_view request);

private:
    class Params;

    struct Sheet
    {
        int id = 0;
        std::string name;
        std::shared_ptr<TableModel> model;
        ColumnFormats formats;
        std::string view_error; // the last failed view, until another is asked for
        unsigned view_error_id = 0;
        Subscription subscription; // declared last: reset before the model goes
    };

    // An export or a Group By, run on a thread of its own.
    struct Job
    {
        std::string kind; // "export" or "groupBy"
        std::atomic<size_t> rows = 0;
        std::atomic<bool> done = false;
        std::mutex mutex; // guards the members below
        std::string error;
        std::filesystem::path file;            // export: the file written
        std::string file_name;                 // export: the name to save it under
        std::optional<ResultSheet> group_rows; // groupBy: until the result sheet is added
        std::string sheet;                     // groupBy: the result sheet
        std::jthread thread;                   // declared last: stopped and joined first
    };

    using Method = void (WebSession::*)(const Params &, JsonWriter &);
    static const std::map<std::string_view, Method> &methods();

    EventSink sink_;
    std::filesystem::path scratch_;
    std::filesystem::path path_; // the open file
    std::shared_ptr<TableBook> book_;
    std::vector<std::unique_ptr<Sheet>> sheets_; // destroyed before book_ by close()
    int next_sheet_id_ = 1;
    std::optional<std::vector<SqlTable>> schema_; // for completion, until the next query
    std::unique_ptr<Job> job_;
    size_t next_export_ = 1;

    void close();
    Sheet &sheet(int id);
    Sheet &sheet(const Params &params);
    // Forgets the sheets of that name, whose models a query or a Group By
    // replaced (TableModel::retire()).
    void dropSheets(const std::string &name);
    void writeSheetNames(JsonWriter &json) const;
    void stopJob();

    void open(const Params &params, JsonWriter &result);
    void closeBook(const Params &params, JsonWriter &result);
    void openSheet(const Params &params, JsonWriter &result);
    void info(const Params &params, JsonWriter &result);
    void writeSheetInfo(JsonWriter &result, Sheet &sheet);
    void rows(const Params &params, JsonWriter &result);
    void setView(const Params &params, JsonWriter &result);
    void sortCycle(const Params &params, JsonWriter &result);
    void search(const Params &params, JsonWriter &result);
    void cancelSearch(const Params &params, JsonWriter &result);
    void searchState(const Params &params, JsonWriter &result);
    void findHit(const Params &params, JsonWriter &result);
    void continueReading(const Params &params, JsonWriter &result);
    void stopReading(const Params &params, JsonWriter &result);
    void copy(const Params &params, JsonWriter &result);
    void goTo(const Params &params, JsonWriter &result);
    void filterOps(const Params &params, JsonWriter &result);
    void cellFilter(const Params &params, JsonWriter &result);
    void setFormat(const Params &params, JsonWriter &result);
    void dateStyles(const Params &params, JsonWriter &result);
    void sql(const Params &params, JsonWriter &result);
    void sqlSchema(const Params &params, JsonWriter &result);
    void completeSql(const Params &params, JsonWriter &result);
    void startStats(const Params &params, JsonWriter &result);
    void statsState(const Params &params, JsonWriter &result);
    void startHistogram(const Params &params, JsonWriter &result);
    void histogramState(const Params &params, JsonWriter &result);
    void histogramFilter(const Params &params, JsonWriter &result);
    void defaultScatter(const Params &params, JsonWriter &result);
    void startScatter(const Params &params, JsonWriter &result);
    void scatterState(const Params &params, JsonWriter &result);
    void scatterFilter(const Params &params, JsonWriter &result);
    void exportFormats(const Params &params, JsonWriter &result);
    void startExport(const Params &params, JsonWriter &result);
    void aggregates(const Params &params, JsonWriter &result);
    void startGroupBy(const Params &params, JsonWriter &result);
    void jobState(const Params &params, JsonWriter &result);
    void cancelJob(const Params &params, JsonWriter &result);
};

#endif // _WEB_SESSION_HXX_
