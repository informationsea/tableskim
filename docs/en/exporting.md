# Exporting and Converting

[← Contents](README.md)

Outside [edit mode](editing.md), TableSkim never changes the files it
opens, but it can write what you see to a new file. The view is exported
with any unsaved edits.

| Feature                    | Source                         | Output                          | What is written                         |
| -------------------------- | ------------------------------ | ------------------------------- | --------------------------------------- |
| Export View                | Any sheet, including SQL results | CSV, TSV, Excel, Parquet      | The sheet as shown: sorted, filtered, without hidden columns |
| Convert to Parquet / DuckDB | CSV, TSV, VCF and other text files | Parquet, DuckDB             | The whole file, in file order           |

## Export View

- GUI: **File → Export View...** (`Ctrl+Shift+E`).
- Terminal UI: `w`.
- Web app: **Export**; choose the format in the dialog, and the browser
  saves the file into its downloads.

Choose a file name; its extension selects the format:

| Extension        | Format                                                                 |
| ---------------- | ---------------------------------------------------------------------- |
| `.csv`           | Comma-separated text, UTF-8                                             |
| `.tsv`, `.tab`   | Tab-separated text, UTF-8                                               |
| `.xlsx`          | Excel workbook                                                          |
| `.parquet`       | Parquet                                                                 |

If the name has none of these extensions, the GUI adds the extension of the
type chosen in the file dialog, and the terminal UI refuses it with the
message "Choose a name ending in .csv, .tsv, .xlsx or .parquet".

The suggested name is the source file name with `-export` added, for
example `sales-export.csv`; for a file with several sheets the sheet name
is included, as in `book-Sheet1-export.xlsx`.

What is written:

- Only the rows that pass the **filter**, in the current **sort** order.
- All columns except **hidden** ones.
- **CSV / TSV**: each cell as it is shown, quoted only where needed, with a
  header row if the sheet has column names.
- **Excel / Parquet**: numbers, booleans, dates and times keep their types.
  A column of text (for example from a CSV file) gets the narrowest type
  that fits every value, so a column of numeric text becomes a numeric
  column and a column of ISO dates becomes a date column. Parquet columns
  are named after the header, or `A`, `B`, `C`, ... without one.

If a column mixes integers and decimals, it stays text when converting to
floating point would change an integer's value. This also applies to
Convert to Parquet / DuckDB.

The export runs in the background with a progress display; **Cancel**
(GUI) or `Esc` (TUI) stops it. The target file is replaced only when it has
been written completely, so a cancelled export leaves an existing file
untouched. If the sheet's sort or filter changes during the export, the
export fails and can be started again.

## Convert to Parquet / DuckDB

Text files have no index, so every sort or filter of a large CSV file reads
all its rows into memory. Converting it once to Parquet or DuckDB makes
later work fast: those formats are sorted and filtered inside DuckDB, and
they can be queried with [SQL](sql.md).

- GUI: **File → Convert to Parquet / DuckDB...**. Choose the target; the
  extension (`.parquet`, or `.duckdb` / `.ddb`) selects the format. The
  converted file is opened when the conversion is done.
- Terminal UI: `P` (Parquet) or `D` (DuckDB) writes the file next to the
  original, for example `data.csv.gz` → `data.parquet` or `data.duckdb`,
  and opens it.
- Web app: **Convert**; choose Parquet or DuckDB, and the browser saves
  the file, named after the source, into its downloads.

What is written:

- The whole file in file order: the sort and filter of the view are **not**
  applied (use Export View for that).
- The current **header**, **`##` line** and **text encoding** choices are
  used, so the result has the same rows and columns as shown.
- Each column gets the narrowest type every value fits: integer, decimal,
  boolean, date, timestamp, or text. Empty cells become NULL in typed
  columns.
- A DuckDB file holds one table named after the source file.

As for exports, the conversion can be cancelled, and the target file is
replaced only when it is complete. The source file itself cannot be chosen
as the target.
