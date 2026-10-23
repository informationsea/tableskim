#ifndef _TUI_SEARCH_HXX_
#define _TUI_SEARCH_HXX_

#include <cstddef>
#include <optional>

#include "table.hxx"

struct SearchHitLookup
{
    std::optional<size_t> index; // into SearchState::hits
    bool wrapped = false;        // went past the last (or first) hit
    bool pending = false;        // not found yet, but the search is still running
};

// The hit after (forward) or before `from` in view order, like vim's n / N.
// With include_from, a hit at `from` itself counts as the next one. Wraps
// around once the search is done; until then, a hit that may still be found
// is reported as pending.
SearchHitLookup findSearchHit(const SearchState &state, CellPosition from, bool forward, bool include_from = false);

#endif // _TUI_SEARCH_HXX_
