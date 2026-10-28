// The WebAssembly module of the web frontend (web/app/): a WebSession on a
// thread of its own (main(), run by -sPROXY_TO_PTHREAD), fed with the
// requests that wasm_worker_pre.js receives from the page.
//
// Only the thread running the runtime (the worker's own) may post to the
// page and touch the file systems, and the core's threads read files
// through it: so it never waits for the session, which does all the work.

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include "web_json.hxx"
#include "web_session.hxx"

EM_JS(void, tskPostReply, (const char *json), { Module['tskReply'](UTF8ToString(json)); });
EM_JS(void, tskPostEvents, (const char *json), { Module['tskEvents'](UTF8ToString(json)); });
EM_JS(void, tskPostReady, (), { Module['tskReady'](); });

namespace
{
    std::mutex request_mutex;
    std::condition_variable request_cv;
    std::deque<std::string> requests;

    // Events are coalesced: a sheet being read reports every block, and the
    // page only needs to know that it should look again.
    std::mutex event_mutex;
    std::map<int, unsigned> pending_events;
    bool events_scheduled = false;

    void runOnRuntimeThread(void (*function)(void *), void *argument)
    {
        emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), function, argument);
    }

    void postReply(void *json)
    {
        tskPostReply(static_cast<const char *>(json));
        std::free(json);
    }

    void postEvents(void *)
    {
        std::map<int, unsigned> events;
        {
            std::lock_guard lock(event_mutex);
            events.swap(pending_events);
            events_scheduled = false;
        }
        JsonWriter json;
        json.beginObject();
        for (const auto &[sheet, bits] : events)
            json.key(std::to_string(sheet)).value(static_cast<size_t>(bits));
        json.endObject();
        tskPostEvents(json.text().c_str());
    }

    void postReady(void *)
    {
        tskPostReady();
    }

    void queueEvents(int sheet, unsigned bits)
    {
        std::lock_guard lock(event_mutex);
        pending_events[sheet] |= bits;
        if (!events_scheduled)
        {
            events_scheduled = true;
            runOnRuntimeThread(postEvents, nullptr);
        }
    }
}

// Takes a request (JSON, allocated with malloc) from the page.
extern "C" EMSCRIPTEN_KEEPALIVE void tskPost(char *request)
{
    {
        std::lock_guard lock(request_mutex);
        requests.emplace_back(request);
    }
    std::free(request);
    request_cv.notify_one();
}

int main()
{
    WebSession session(queueEvents);
    runOnRuntimeThread(postReady, nullptr);
    for (;;)
    {
        std::string request;
        {
            std::unique_lock lock(request_mutex);
            request_cv.wait(lock, []
                            { return !requests.empty(); });
            request = std::move(requests.front());
            requests.pop_front();
        }
        const std::string reply = session.handle(request);
        char *copy = static_cast<char *>(std::malloc(reply.size() + 1));
        reply.copy(copy, reply.size());
        copy[reply.size()] = '\0';
        runOnRuntimeThread(postReply, copy);
    }
}
