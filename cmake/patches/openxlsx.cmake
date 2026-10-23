# Run in OpenXLSX's source directory at every configure (see Dependencies.cmake).
# Each change is made only if its marker is missing, and the file is written
# only when something changed, so running it again neither patches twice nor
# rebuilds.
#
# - OpenXLSX 0.5.1 reads a number as an integer unless it contains '.', "E-"
#   or "e-", and then parses it with as_llong(): "1e+06" (as written by Excel
#   and by TableSkim's own exporter) becomes 1, and integers beyond int64
#   saturate. Any other number is made a Float.
# - An inline string (t="inlineStr") is read from <is><t> only, so rich text
#   (<is><r><t>...) comes back empty. Its runs are joined, as OpenXLSX does
#   for shared strings.

set(file OpenXLSX/sources/XLCellValue.cpp)
file(READ ${file} content)
set(original "${content}")

function(tableskim_patch marker from to)
    string(FIND "${content}" "${marker}" found)
    if(NOT found EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${from}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Could not patch ${file} (${marker}); check the OpenXLSX version")
    endif()
    string(REPLACE "${from}" "${to}" content "${content}")
    set(content "${content}" PARENT_SCOPE)
endfunction()

set(anchor [[#include "XLXmlParser.hpp"              // pugixml wrapper
]])

tableskim_patch("static bool tableskimIsFloat" "${anchor}" "${anchor}
#include <cerrno>
#include <cstdlib>

// TableSkim: an exponent or a value beyond int64 makes a number a Float.
static bool tableskimIsFloat(const std::string& number)
{
    if (number.find_first_not_of(\"+-0123456789\") != std::string::npos) return true;
    errno = 0;
    std::strtoll(number.c_str(), nullptr, 10);
    return errno == ERANGE;
}
")
tableskim_patch("tableskimIsFloat(numberString)"
    [[numberString.find('.') != std::string::npos || numberString.find("E-") != std::string::npos || numberString.find("e-") != std::string::npos)]]
    [[tableskimIsFloat(numberString))]])

tableskim_patch("static std::string tableskimInlineText" "${anchor}" "${anchor}
// TableSkim: the text of an inline string, rich text runs included.
static std::string tableskimInlineText(OpenXLSX::XMLNode is)
{
    std::string text;
    for (OpenXLSX::XMLNode elem = is.first_child_of_type(pugi::node_element); not elem.empty();
         elem = elem.next_sibling_of_type(pugi::node_element)) {
        const std::string name = elem.name();
        if (name == \"t\") text += elem.text().get();
        else if (name == \"r\") text += elem.child(\"t\").text().get();
    }
    return text;
}
")
tableskim_patch("tableskimInlineText(m_cellNode"
    [[return XLCellValue { m_cellNode->child("is").child("t").text().get() };]]
    [[return XLCellValue { tableskimInlineText(m_cellNode->child("is")) };]])

if(NOT content STREQUAL original)
    file(WRITE ${file} "${content}")
endif()
