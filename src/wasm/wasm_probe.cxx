// Feasibility probe of the WebAssembly build: opens one file through the
// normal backends, reads it to the end, sorts it and (where the format
// supports SQL) runs a query, printing how long each step took. Run by
// Node.js or, in a browser, by web/probe/ (see src/wasm/README.md).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

#include "cell_format.hxx"
#include "file_util.hxx"
#include "table.hxx"

namespace
{
    using Clock = std::chrono::steady_clock;

    double secondsSince(Clock::time_point start)
    {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    bool waitFor(const std::function<bool()> &condition, std::chrono::seconds timeout = std::chrono::seconds(600))
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline)
        {
            if (condition())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return condition();
    }

    // The size of the WebAssembly memory, which only grows.
    size_t heapMiB()
    {
#ifdef __EMSCRIPTEN__
        return emscripten_get_heap_size() >> 20;
#else
        return 0;
#endif
    }

    void report(const std::string &step, double seconds, const std::string &detail = {})
    {
        std::printf("probe: %-12s %8.3f s  heap %5zu MiB  %s\n", step.c_str(), seconds, heapMiB(), detail.c_str());
        std::fflush(stdout);
    }

    std::string rowText(TableModel &model, size_t row)
    {
        std::string text;
        for (size_t column = 0; column < model.columnCount() && column < 6; ++column)
        {
            const CellRef ref = model.peek(row, column);
            text += column ? " | " : "";
            text += ref.state == CellState::Ready ? cellDisplayText(*ref.value, 24) : "";
        }
        return text;
    }

    int probe(const std::string &path, const std::string &sql)
    {
        std::printf("probe: file %s\n", path.c_str());
        std::fflush(stdout);
        const auto start = Clock::now();
        auto book = TableBook::open(utf8Path(path));
        const auto names = book->sheetNames();
        report("open", secondsSince(start), std::to_string(names.size()) + " sheet(s), first \"" + names.front() + "\"");

        auto model = book->sheet(names.front());
        auto step = Clock::now();
        model->requestRange(0, 20);
        if (!waitFor([&]
                     { return model->state() != ModelState::Opening && model->peek(0, 0).state != CellState::Loading; }))
            return std::printf("probe: timed out waiting for the first rows\n"), 1;
        if (model->state() == ModelState::Error)
            return std::printf("probe: error: %s\n", model->errorMessage().c_str()), 1;
        report("first rows", secondsSince(step), std::to_string(model->columnCount()) + " columns");
        std::printf("probe:   header: ");
        for (size_t column = 0; column < model->columnCount() && column < 6; ++column)
            std::printf("%s%s", column ? " | " : "", model->columnName(column).c_str());
        std::printf("\nprobe:   row 0:  %s\n", rowText(*model, 0).c_str());

        // Statistics scan every row of the view, which reads a sequential
        // source to its end.
        step = Clock::now();
        model->startColumnStats(0);
        for (;;)
        {
            if (!waitFor([&]
                         { return model->columnStatsState().done || model->memoryCheckpoint(); }))
                return std::printf("probe: timed out reading the rows\n"), 1;
            const auto checkpoint = model->memoryCheckpoint();
            if (!checkpoint)
                break;
            // Read on, as a user would, to see what the whole file costs.
            report("checkpoint", secondsSince(step),
                   std::to_string(checkpoint->rows) + " rows, " + std::to_string(checkpoint->bytes >> 20) + " MiB of cells");
            model->continueReading();
        }
        const auto stats = model->columnStatsState();
        report("read all", secondsSince(step),
               rowCountText(model->rowCount()) + ", column 0: " + std::to_string(stats.stats.distinct) + " distinct");

        step = Clock::now();
        const auto capabilities = model->viewCapabilities();
        if (capabilities.sort)
        {
            ViewSpec spec;
            spec.sort.push_back({0, false});
            model->setView(spec);
            if (!waitFor([&]
                         { return model->view() == spec; }))
                return std::printf("probe: timed out sorting\n"), 1;
            model->requestRange(0, 20);
            waitFor([&]
                    { return model->peek(0, 0).state != CellState::Loading; });
            report("sort desc", secondsSince(step), "row 0: " + rowText(*model, 0));
        }

        if (!sql.empty() && book->supportsSql())
        {
            step = Clock::now();
            std::atomic<bool> done = false;
            TableBook::SqlResult result;
            book->executeSql(sql, [&](const TableBook::SqlResult &r)
                             { result = r; done = true; });
            waitFor([&]
                    { return done.load(); });
            if (!result.ok)
                return std::printf("probe: SQL error: %s\n", result.message.c_str()), 1;
            auto sql_model = book->sheet(result.sheet);
            sql_model->requestRange(0, 20);
            waitFor([&]
                    { return sql_model->state() != ModelState::Opening && sql_model->peek(0, 0).state != CellState::Loading; });
            report("sql", secondsSince(step), "row 0: " + rowText(*sql_model, 0));
        }

        report("total", secondsSince(start));
        return 0;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: tableskim-wasm-probe FILE [SQL]\n");
        return 2;
    }
    try
    {
        return probe(argv[1], argc > 2 ? argv[2] : "");
    }
    catch (const std::exception &e)
    {
        std::printf("probe: error: %s\n", e.what());
        return 1;
    }
}
