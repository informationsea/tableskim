#include "gui_convert.hxx"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_ask.H>
#include <atomic>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "spdlog/spdlog.h"

#include "file_util.hxx"
#include "gui_table_view.hxx"
#include "gui_theme.hxx"

namespace
{
    const int DIALOG_WIDTH = 360;
    const int DIALOG_HEIGHT = 100;
    const int MARGIN = 10;
    const int BUTTON_WIDTH = 80;
    const int BUTTON_HEIGHT = 25;
    // How often the dialog shows the row count.
    const double POLL_INTERVAL = 0.1;

    // Written by the worker; result and error are published by `done`.
    struct ConversionState
    {
        std::atomic<size_t> rows{0};
        std::atomic<bool> done{false};
        std::optional<size_t> result;
        std::string error;
    };

    void requestStop(Fl_Widget *, void *data)
    {
        static_cast<std::stop_source *>(data)->request_stop();
    }
} // namespace

namespace
{
    // Runs `task` on a worker thread behind the dialog. `action` is shown
    // above the row count, e.g. "Converting data.csv...".
    bool runWithProgress(const std::string &title, const std::string &action, const std::string &failure,
                         const std::function<std::optional<size_t>(std::stop_token, const std::function<void(size_t)> &)> &task)
    {
        ConversionState state;
        std::stop_source stop;

        Fl_Double_Window dialog(DIALOG_WIDTH, DIALOG_HEIGHT, title.c_str());
        Fl_Box label(MARGIN, MARGIN, DIALOG_WIDTH - 2 * MARGIN, DIALOG_HEIGHT - 3 * MARGIN - BUTTON_HEIGHT);
        label.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
        Fl_Button cancel(DIALOG_WIDTH - MARGIN - BUTTON_WIDTH, DIALOG_HEIGHT - MARGIN - BUTTON_HEIGHT,
                         BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel");
        cancel.callback(requestStop, &stop);
        dialog.end();
        // Closing the dialog cancels; it stays up until the worker has stopped.
        dialog.callback(requestStop, &stop);
        dialog.set_modal();
        dialog.show();

        std::jthread worker([&state, &task, token = stop.get_token()]
                            {
            try
            {
                state.result = task(token, [&state](size_t rows)
                                    { state.rows = rows; });
            }
            catch (const std::exception &e)
            {
                state.error = e.what();
            }
            state.done = true; });

        {
            // Keeps Auto Revert from reopening files while the dialog is up.
            NestedLoopGuard guard;
            std::string text;
            while (!state.done)
            {
                const std::string next = stop.stop_requested()
                                             ? "Cancelling..."
                                             : escapeLabel(action) + "\n" + std::to_string(state.rows.load()) +
                                                   " rows read";
                if (next != text)
                {
                    text = next;
                    label.copy_label(text.c_str());
                    dialog.redraw();
                }
                Fl::wait(POLL_INTERVAL);
            }
        }
        worker.join();
        dialog.hide();

        if (!state.error.empty())
        {
            spdlog::error("{}: {}", failure, state.error);
            NestedLoopGuard guard;
            // A message may name a file or quote SQL: '@' would start a symbol.
            fl_alert("%s:\n%s", escapeLabel(failure).c_str(), escapeLabel(state.error).c_str());
            return false;
        }
        if (!state.result)
        {
            spdlog::debug("{} canceled", title);
            return false;
        }
        spdlog::debug("{}: {} rows", title, *state.result);
        return true;
    }
} // namespace

bool runConversion(const std::filesystem::path &source, const std::filesystem::path &target, ExportFormat format,
                   const OpenOptions &options)
{
    const std::string format_name = exportFormatName(format);
    return runWithProgress("Convert to " + format_name, "Converting " + utf8String(source.filename()) + "...",
                           "Failed to convert to " + format_name,
                           [&](std::stop_token stop, const std::function<void(size_t)> &progress)
                           { return exportTextFile(source, target, format, options, stop, progress); });
}

bool runViewExport(const std::shared_ptr<TableModel> &model, const std::filesystem::path &target,
                   ViewExportFormat format, const ViewExportOptions &options)
{
    const std::string format_name = viewExportFormatName(format);
    return runWithProgress("Export View as " + format_name, "Exporting to " + utf8String(target.filename()) + "...",
                           "Failed to export the view as " + format_name,
                           [&](std::stop_token stop, const std::function<void(size_t)> &progress)
                           { return exportView(model, target, format, options, stop, progress); });
}

TableBook::SaveResult runSave(TableBook &book, const std::filesystem::path &target)
{
    struct SaveState
    {
        std::atomic<bool> done{false};
        TableBook::SaveResult result;
    };
    auto state = std::make_shared<SaveState>();

    const int height = DIALOG_HEIGHT - BUTTON_HEIGHT - MARGIN;
    Fl_Double_Window dialog(DIALOG_WIDTH, height, "Save");
    Fl_Box label(MARGIN, MARGIN, DIALOG_WIDTH - 2 * MARGIN, height - 2 * MARGIN);
    label.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    label.copy_label(escapeLabel("Saving " + utf8String(target.filename()) + "...").c_str());
    dialog.end();
    // The file is written on the book's worker, which cannot be interrupted.
    dialog.callback([](Fl_Widget *, void *) {});
    dialog.set_modal();
    dialog.show();

    book.saveEdits(target, [state](const TableBook::SaveResult &result)
                   {
        state->result = result;
        state->done = true;
        Fl::awake(); });
    {
        NestedLoopGuard guard;
        while (!state->done)
            Fl::wait(POLL_INTERVAL);
    }
    dialog.hide();
    return state->result;
}
