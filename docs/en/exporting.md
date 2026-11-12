# Exporting and Converting

[← Contents](README.md)

Outside [edit mode](editing.md), TableSkim never changes the files it
opens, but it can write what you see to a new file. The view is exported
with any unsaved edits.

| Feature                    | Source                         | Output                          | What is written                         |
| -------------------------- | ------------------------------ | ------------------------------- | --------------------------------------- |
| Export View                | Any sheet, including SQL results | CSV, TSV, Excel, Parquet      | The sheet as shown: sorted, filtered, without hidden columns |
| Convert to Parquet / DuckDB | CSV, TSV, VCF and other text files | Parquet, DuckDB             | The whole file, in file order           |
| Analysis code              | The file's sheets (not SQL results) | R, Python                    | Code that reads the file and repeats the filter, chart or Group By |

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

In the GUI and terminal UI, unsaved changes stay open after conversion;
the converted file is saved without opening it.

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

## Analysis code for R and Python

TableSkim can write code that repeats an analysis in R or Python, as a
starting point for a script or a notebook: the code reads the file, applies
the view's filter, and then draws the histogram or scatter plot, computes
the Group By, or (for the sheet itself) sorts the rows and leaves out the
hidden columns.

- GUI: **Save Code...** in the histogram and scatter plot windows; **File →
  Export Analysis Code...** for the current sheet, or, on the **Group By**
  sheet, for the Group By that made it. The type chosen in the file dialog
  picks the language.
- Terminal UI: `K` in the sheet, the histogram (`B`) or the density (`M`);
  the prompt suggests a name next to the file, and `Tab` picks the
  language.
- Web app: **Code** in the toolbar, and **Code…** in the chart dialogs;
  the dialog shows the code, with **Copy** and **Save…**. The code reads
  the file by its name: set `path` to where it is.

Three languages are written:

| Language                      | Libraries                                   | Suggested name             |
| ----------------------------- | ------------------------------------------- | -------------------------- |
| R (tidyverse)                 | readr, dplyr, ggplot2 (readxl, arrow, jsonlite, DBI for other formats) | `sales-histogram.R`        |
| Python (polars + plotnine)    | polars, plotnine (duckdb, fastexcel as needed) | `sales-histogram-polars.py` |
| Python (pandas + matplotlib)  | pandas, matplotlib (duckdb, openpyxl as needed) | `sales-histogram-pandas.py` |

For example, a histogram of `Salary` stacked by `Active`, over the rows
with `Age` above 40, becomes in R:

```r
library(tidyverse)

path <- "/data/normal.csv"
df <- read_csv(path)

df <- df |>
  filter(Age > 40)

p <- ggplot(df, aes(x = Salary, fill = factor(Active))) +
  geom_histogram(bins = 30) +
  labs(y = "rows", fill = "Active")
print(p)
# ggsave("plot.png", p)
```

The code is kept short rather than exact:

- The libraries read the file with their own rules: they guess each
  column's type, and readr trims spaces around values. A filter compares a
  value with a column of numbers, dates or booleans as such, and with any
  other column as text.
- In R and polars, a comparison with an empty cell drops the row, except
  "not equal to"; TableSkim compares an empty cell as empty text.
- Charts keep the libraries' own colours, bins and axes; a text X makes box
  plots, and a histogram of text shows its most frequent values.
- A Group By counts the rows of each group (`count`) and names its columns
  as TableSkim does (`Salary sum`); sums, means and standard deviations of
  text columns are left out.

SQL results have no file to read, so they get no code (nor does a Group By
of them).

