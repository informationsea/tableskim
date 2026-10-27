#ifndef _WEB_SESSION_HXX_
#define _WEB_SESSION_HXX_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "table.hxx"

// The web frontend's view of the core: one book at a time and the sheets
// shown from it, driven by JSON requests
//   {"id": 1, "method": "rows", "params": {...}}
// answered by {"id": 1, "result": {...}} or {"id": 1, "error": "..."}.
// See src/wasm/README.md for the methods. handle() is called on one thread
// (the session thread of the WebAssembly module); it never waits for the
// book's worker, so a reply may say that rows are still loading: the sink
// then reports the change and the frontend asks again.
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
    };
    // Called on the book's worker thread, once per event: coalesce them.
    using EventSink = std::function<void(int sheet, unsigned events)>;

    explicit WebSession(EventSink sink);
    ~WebSession();
    WebSession(const WebSession &) = delete;
    WebSession &operator=(const WebSession &) = delete;

    std::string handle(std::string_view request);

private:
    struct Sheet
    {
        int id = 0;
        std::string name;
        std::shared_ptr<TableModel> model;
        Subscription subscription; // declared last: reset before the model goes
    };

    EventSink sink_;
    std::shared_ptr<TableBook> book_;
    std::vector<std::unique_ptr<Sheet>> sheets_; // destroyed before book_ by close()
    int next_sheet_id_ = 1;

    void close();
    Sheet &sheet(int id);
};

#endif // _WEB_SESSION_HXX_
