#include "search_hits.hxx"

#include <algorithm>

SearchHitLookup findSearchHit(const SearchState &state, CellPosition from, bool forward, bool include_from)
{
    const auto &hits = state.hits;
    if (forward)
    {
        const auto it = include_from ? std::lower_bound(hits.begin(), hits.end(), from, searchHitBefore)
                                     : std::upper_bound(hits.begin(), hits.end(), from, searchHitBefore);
        if (it != hits.end())
            return {static_cast<size_t>(it - hits.begin())};
        if (!state.done)
            return {.pending = true};
        if (hits.empty())
            return {};
        return {.index = 0, .wrapped = true};
    }

    const auto it = include_from ? std::upper_bound(hits.begin(), hits.end(), from, searchHitBefore)
                                 : std::lower_bound(hits.begin(), hits.end(), from, searchHitBefore);
    if (it != hits.begin())
        return {static_cast<size_t>(it - hits.begin()) - 1};
    // Wrapping to the last hit needs the whole sheet scanned.
    if (!state.done)
        return {.pending = true};
    if (hits.empty())
        return {};
    return {.index = hits.size() - 1, .wrapped = true};
}
