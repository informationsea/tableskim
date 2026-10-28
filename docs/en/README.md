# TableSkim User Manual

TableSkim is a lightweight, fast viewer for tabular files. Open a large CSV
file, an Excel workbook, a Parquet or JSON file, or a DuckDB / SQLite
database, and start scrolling immediately: rows are loaded in the background
while the window stays responsive.

TableSkim has two user interfaces in one program:

- a **graphical interface (GUI)** for desktop use, and
- a **terminal interface (TUI)** for use in a terminal, for example over SSH.

Both offer the same features: sorting, filtering, searching, column
statistics, histograms, scatter plots, grouping (Group By), SQL, hiding and
freezing columns, number and date formats, following a growing file
(`tail -f`), exporting, and editing cells of CSV / TSV and Excel files in
edit mode.

A **web app** runs the same core in a web browser, without installing
anything and without uploading the files it opens; it has these features
except editing and following a file (see [Using the Web App](web.md)).

![The TableSkim window](../images/gui-main-window.png)

## Contents

1. [Getting Started](getting-started.md): installing, starting TableSkim and
   command-line options
2. [Supported File Formats](file-formats.md): what TableSkim can open, text
   encodings and compression
3. [Using the GUI](gui.md): the window, menus, sidebar and all GUI features
4. [Using the Terminal UI](tui.md): the terminal interface and its keys
5. [Using the Web App](web.md): setting up and using TableSkim in a web
   browser
6. [Sorting, Filtering, Searching and Statistics](filtering.md): how
   comparisons work, how histogram bins are chosen and how rows are
   grouped, shared by both interfaces
7. [Charts and Group By](charts.md): which chart answers which question,
   how to read histograms, scatter plots and box plots, grouping rows into
   tables, and saving charts as images, with examples
8. [Running SQL](sql.md): querying DuckDB, SQLite, Parquet and JSON files
9. [Exporting and Converting](exporting.md): saving the view and converting
   text files to Parquet / DuckDB
10. [Editing Cells](editing.md): changing cells of CSV / TSV and Excel files
   in edit mode, and saving them
11. [Keyboard Shortcuts](keyboard-shortcuts.md): a quick reference for both
    interfaces and the web app
