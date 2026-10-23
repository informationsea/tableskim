#ifndef _TEST_HELPERS_HXX_
#define _TEST_HELPERS_HXX_

#include <chrono>
#include <functional>
#include <thread>

#include "table.hxx"

// Polls until the condition holds or the timeout expires.
inline bool waitFor(const std::function<bool()> &condition, std::chrono::milliseconds timeout = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

// Peeks until the cell is no longer Loading.
inline CellRef waitForCell(TableModel &model, size_t row, size_t col)
{
    CellRef ref;
    waitFor([&]
            { ref = model.peek(row, col); return ref.state != CellState::Loading; });
    return ref;
}

inline std::string cellText(TableModel &model, size_t row, size_t col)
{
    const CellRef ref = waitForCell(model, row, col);
    return ref.state == CellState::Ready ? formatCell(*ref.value) : std::string("<state ") + std::to_string(static_cast<int>(ref.state)) + ">";
}

#endif // _TEST_HELPERS_HXX_
