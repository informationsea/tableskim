#include <catch2/catch_test_macros.hpp>

#include "tui_search.hxx"

namespace
{
    SearchState searchState(std::vector<CellPosition> hits, bool done)
    {
        SearchState state;
        state.active = true;
        state.done = done;
        state.hits = std::move(hits);
        return state;
    }
} // namespace

TEST_CASE("findSearchHit moves forward and backward in view order")
{
    const SearchState state = searchState({{1, 2}, {3, 0}, {3, 4}, {7, 1}}, true);

    auto next = findSearchHit(state, {3, 0}, true);
    CHECK(next.index == 2u);
    CHECK_FALSE(next.wrapped);
    // Between hits, and on a hit with include_from.
    CHECK(findSearchHit(state, {2, 9}, true).index == 1u);
    CHECK(findSearchHit(state, {3, 0}, true, true).index == 1u);

    auto previous = findSearchHit(state, {3, 4}, false);
    CHECK(previous.index == 1u);
    CHECK(findSearchHit(state, {3, 3}, false).index == 1u);
    CHECK(findSearchHit(state, {3, 4}, false, true).index == 2u);
}

TEST_CASE("findSearchHit wraps around once the search is done")
{
    const SearchState state = searchState({{1, 2}, {7, 1}}, true);

    auto next = findSearchHit(state, {7, 1}, true);
    CHECK(next.index == 0u);
    CHECK(next.wrapped);

    auto previous = findSearchHit(state, {1, 2}, false);
    CHECK(previous.index == 1u);
    CHECK(previous.wrapped);

    const SearchState empty = searchState({}, true);
    auto none = findSearchHit(empty, {0, 0}, true);
    CHECK_FALSE(none.index);
    CHECK_FALSE(none.pending);
}

TEST_CASE("findSearchHit waits for a running search instead of wrapping")
{
    const SearchState state = searchState({{1, 2}, {7, 1}}, false);

    CHECK(findSearchHit(state, {0, 0}, true).index == 0u);
    CHECK(findSearchHit(state, {7, 1}, true).pending);
    // Hits before the cursor are all known, but the last one is not yet.
    CHECK(findSearchHit(state, {8, 0}, false).index == 1u);
    CHECK(findSearchHit(state, {1, 2}, false).pending);
}
