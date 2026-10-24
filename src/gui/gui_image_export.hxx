#ifndef _GUI_IMAGE_EXPORT_HXX_
#define _GUI_IMAGE_EXPORT_HXX_

#include <filesystem>
#include <string>

class Fl_Widget;

// Writes what the widget draws, at its size, as a PNG or (by the
// extension .svg) an SVG file. Returns the error, empty on success.
std::string writeWidgetImage(Fl_Widget *widget, const std::filesystem::path &target);

// Asks for a PNG or SVG file (named `file_name` at first, without an
// extension) and writes the widget's image to it; errors are shown.
void saveWidgetImage(Fl_Widget *widget, const std::string &file_name);

// A file name from a title such as "Histogram - Salary": characters that
// file systems refuse become "_".
std::string imageFileName(const std::string &title);

#endif // _GUI_IMAGE_EXPORT_HXX_
