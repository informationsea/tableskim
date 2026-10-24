#ifndef _GUI_GROUP_BY_HXX_
#define _GUI_GROUP_BY_HXX_

#include <optional>
#include <vector>

#include "cell_format.hxx"
#include "group_by.hxx"

class TableModel;

// Asks which columns of the sheet to group by and which to aggregate, and
// how, in a modal dialog. `initial` gives the choices checked at first
// (columns outside the sheet are ignored); hidden columns are not listed.
// nullopt if cancelled.
std::optional<GroupBySpec> askGroupBySpec(const TableModel &model, const HiddenColumns &hidden,
                                          const GroupBySpec &initial);

#endif // _GUI_GROUP_BY_HXX_
