# TableSkim User Manual

TableSkim is a lightweight, fast viewer for tabular files. Open a large CSV
file, an Excel workbook, a Parquet or JSON file, or a DuckDB / SQLite
database, and start scrolling immediately: rows are loaded in the background
while the window stays responsive.

TableSkim has two user interfaces in one program:

- a **graphical interface (GUI)** for desktop use, and
- a **terminal interface (TUI)** for use in a terminal, for example over SSH.

Both offer the same features: sorting, filtering, searching, column
statistics, histograms, scatter plots, SQL, hiding and freezing columns, exporting, and
editing cells of CSV / TSV and Excel files in edit mode.

![The TableSkim window](../images/gui-main-window.png)

## Contents

1. [Getting Started](getting-started.md): installing, starting TableSkim and
   command-line options
2. [Supported File Formats](file-formats.md): what TableSkim can open, text
   encodings and compression
3. [Using the GUI](gui.md): the window, menus, sidebar and all GUI features
4. [Using the Terminal UI](tui.md): the terminal interface and its keys
5. [Sorting, Filtering, Searching and Statistics](filtering.md): how
   comparisons work and how histogram bins are chosen, shared by both
   interfaces
6. [Running SQL](sql.md): querying DuckDB, SQLite, Parquet and JSON files
7. [Exporting and Converting](exporting.md): saving the view and converting
   text files to Parquet / DuckDB
8. [Editing Cells](editing.md): changing cells of CSV / TSV and Excel files
   in edit mode, and saving them
9. [Keyboard Shortcuts](keyboard-shortcuts.md): a quick reference for both
   interfaces
