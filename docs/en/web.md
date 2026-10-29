# Using the Web App

[← Contents](README.md)

- [What the web app is](#what-the-web-app-is)
- [Setting it up](#setting-it-up)
- [The page](#the-page)
- [Opening files](#opening-files)
- [Moving around, selecting and copying](#moving-around-selecting-and-copying)
- [Going to a row, cell or column](#going-to-a-row-cell-or-column)
- [The sidebar](#the-sidebar)
- [Sorting](#sorting)
- [Filtering](#filtering)
- [Searching](#searching)
- [Column statistics](#column-statistics)
- [Histograms and scatter plots](#histograms-and-scatter-plots)
- [Group By](#group-by)
- [Hiding and freezing columns](#hiding-and-freezing-columns)
- [Column types and formats](#column-types-and-formats)
- [Sheets](#sheets)
- [Running SQL](#running-sql)
- [Exporting and converting](#exporting-and-converting)
- [Large text files](#large-text-files)
- [Differences from the GUI](#differences-from-the-gui)
- [Keyboard shortcuts](#keyboard-shortcuts)

On macOS, use `Cmd` wherever this page says `Ctrl`.

## What the web app is

The web app is TableSkim running in a web browser. The same core as the GUI
and the terminal UI, compiled to WebAssembly, reads the file inside the
browser: **the files you open are never uploaded**, and nothing needs to be
installed on the computers that use it. It opens every format the other
interfaces open (see [Supported File Formats](file-formats.md)) and has
most of their features; [Differences from the GUI](#differences-from-the-gui)
lists what it lacks.

![The web app with the Content pane](../images/web-main.png)

## Setting it up

The web app is a folder of static files, released as
`tableskim-<version>-web.zip`. Put the folder on any web server, under any
path, and open the folder's URL with the slash at the end, for example
`https://example.com/tools/tableskim/`. No program runs on the server.

- The page must be served over **HTTPS**, or from `http://localhost` /
  `http://127.0.0.1`. Opening `index.html` as a file does not work.
- The core runs on several threads, which browsers allow only in a
  *cross-origin isolated* page. Servers that can should send these headers
  with every file of the folder:

  ```text
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  ```

  Where they cannot be set (GitHub Pages, for one), the page registers a
  service worker that adds them, for its folder only, and reloads once. If
  the page still says it must be served cross-origin isolated, the server
  or the browser does not allow either.
- To try it on your own computer, run `python3 -m http.server 8000` in the
  folder and open `http://localhost:8000/`.

Use a recent browser. The web app is tested in Chromium (Google Chrome,
Microsoft Edge).

To use it without a server of your own, open the latest release's web app
on the TableSkim website: [informationsea.github.io/tableskim/app/](https://informationsea.github.io/tableskim/app/).
It, too, reads the files in your browser only.

## The page

- **Toolbar** at the top: **Open...**, the reading options of the file
  (**First row**, **Encoding**, **Skip ## lines**; shown only when they
  apply), the buttons of the features (**Filter**, **Statistics**,
  **Histogram**, **Scatter**, **Group By**, **SQL**, **Export**,
  **Convert**, **Sidebar**, **Monospace**) and the search box. Buttons
  that do not apply to the file are dimmed; a pressed button shows that
  its panel is open.
- **Sheet tabs** below the toolbar, for files with several sheets.
- **Column headers**: the column name, a mark of what the column seems to
  hold (`#` numbers, `@` dates, `?` booleans) and the sort arrow. A
  filtered column's name is underlined in the accent colour.
- **Row numbers**: the position of the row in the current view.
- **Sidebar** on the right (hidden at first): the Content, Filter and
  Statistics panes.
- **Status bar** at the bottom: the file and its text encoding, the row
  count (`4 rows of 54` when filtered), the filter, and the cursor's row
  and column.

The page follows the light or dark appearance of the system.

## Opening files

- Click **Open...** (`Ctrl+O`) and choose a file, or drop a file anywhere
  on the page.
- The page shows one file at a time; opening another replaces it. To see
  two files side by side, open the web app in two browser tabs.
- For a text file (CSV, TSV, VCF):
  - **First row** chooses whether the first row is the header (**Auto**,
    **Header**, **Data**), as in the GUI. It is shown for Excel files too.
  - **Encoding** shows the detected text encoding and reads the file again
    in another one: UTF-8, UTF-16LE, UTF-16BE or Shift_JIS (CP932).
  - **Skip ## lines** leaves out lines starting with `##`, such as the
    meta-information of VCF files. It is on by itself for VCF files.

  Changing one of them opens the file again and keeps the others. See
  [Supported File Formats](file-formats.md#csv-tsv-and-other-text-files).

## Moving around, selecting and copying

- Arrow keys, `Page Up` / `Page Down`, `Home` / `End` move the cursor;
  `Ctrl+Home` / `Ctrl+End` go to the first / last row. `Ctrl+End` keeps
  following the last row while a text file is still being read.
- Click a cell to select it, `Shift+click` or drag to select a range,
  `Shift` with a movement key to extend the selection. Click a row number
  to select the row. `Ctrl+A` selects everything.
- Drag the right edge of a column header to change the column's width.
- `Ctrl+C`, or **Copy** in the cell menu, copies the selection as
  tab-separated text (hidden columns left out), ready to paste into a
  spreadsheet. Up to 100,000 rows are copied at a time; rows not read yet
  are left out, and the status bar says so.

Right-click a cell or a column header for its menu: copying, quick filters,
sorting, the column's statistics, charts and Group By, formats, hiding and
freezing.

## Going to a row, cell or column

`Ctrl+G` opens **Go To**. Type a row number (`120`), a column letter (`C`),
a header name or a cell (`C120`) and press `Enter`, as in the GUI.

## The sidebar

The **Sidebar** button shows or hides the sidebar. Its tabs:

- **Content**: the whole value of the selected cell (the top-left cell of
  a selection), wrapped to the width of the pane, with its line breaks.
  The grid cuts long values short; this shows them whole. `Enter` or a
  double click on a cell opens it.
- **Filter**: the filter conditions (see [Filtering](#filtering)).
- **Statistics**: the statistics of the cursor's column (see
  [Column statistics](#column-statistics)).

Drag the sidebar's left edge to make it wider; the width is kept in the
browser.

## Sorting

Click a column header to sort by that column: the first click sorts in
ascending order, the second in descending order, and the third restores
the file order. The column menu has **Sort Ascending**, **Sort Descending**
and **Clear Sort**. How values compare is explained in
[Sorting, Filtering, Searching and Statistics](filtering.md#sorting).

## Filtering

The **Filter** button (`Ctrl+Shift+F`) opens the Filter pane of the
sidebar, laid out like the GUI's.

![The Filter pane with two conditions](../images/web-filter-pane.png)

- **Match: All / Any**: show rows that match all conditions (AND) or any
  condition (OR).
- Each condition has a **column**, an **operator** and a **value**. The
  **×** button removes the condition (at once).
- **+ Add condition** adds a condition for the cursor's column.
- **Apply** (or `Enter` in a value field) applies the filter; until then
  the line below says "Press Apply (or Enter) to filter." It then shows
  the result, for example "4 rows of 54", or why the filter cannot be
  applied (an invalid regular expression, for one).
- **Clear** removes all conditions.

**Filter by This Column...** in a column or cell menu opens the pane with
a new condition for that column; **Clear Filter** removes the filter.

**Quick filters**: right-click a cell and choose **Filter: Equal to This
Value** or **Filter: Not Equal to This Value**. Each quick filter is added
to the filter as another condition.

Clicking a bar of a histogram or a cell of a density plot narrows the rows
further. These selections show as **Additional narrowing (N) - Clear**,
ANDed with the conditions even when **Match: Any** is chosen; the button
removes them, and **Clear** removes them with the conditions.

The operators and how values compare are explained in
[Sorting, Filtering, Searching and Statistics](filtering.md#filtering).

## Searching

Type into the search box at the top right (`Ctrl+F` goes there) and press
`Enter`. The cursor moves to the first hit after it, and every hit in view
is marked. `Enter` and `Shift+Enter`, or the **↑** / **↓** buttons, move to
the next and previous hit; `Escape` returns to the grid. **Aa** matches
upper and lower case exactly and **.\*** reads the text as a regular
expression. The box shows how many hits there are so far.

## Column statistics

The **Statistics** button, or **Statistics** in a column menu, shows the
Statistics pane: the number of rows and of empty cells, the number of
distinct values, the minimum and maximum, the mean and standard deviation
of numbers, and the most frequent values of the cursor's column, over the
rows of the view. It
follows the cursor from column to column. See
[Column statistics](filtering.md#column-statistics).

## Histograms and scatter plots

- **Histogram** shows how the values of the cursor's column are
  distributed. Choose the column and the number of bins; **Table** shows
  the counts as a table. Click a bar to narrow the rows to it.

  ![A histogram](../images/web-histogram.png)

- **Scatter** plots two columns against each other: **Points**,
  **Density** or, when X holds text, **Box plots** of each category, with
  the least-squares **Regression line**. Click a point to go to its row,
  click a density cell or a box to narrow the rows to it, or drag a
  rectangle to narrow them to it.

**Save PNG** and **Save SVG** save the chart as shown; the browser saves
the file into its downloads. PNG is drawn at twice the size of the page,
and SVG keeps lines and text sharp at any size.

How to read the charts and which one answers which question is explained in
[Charts and Group By](charts.md).

## Group By

**Group By** groups the rows of the view by the values of the key columns
and computes aggregates (count, distinct, sum, mean, min, max) of other
columns, as in the GUI. Tick each column's **Key** or **Aggregate** box and the aggregates to
compute. The result opens as the sheet **Group By**, which a later Group By
replaces. See [Summarizing in a table: Group By](charts.md#summarizing-in-a-table-group-by).

## Hiding and freezing columns

In a column menu:

- **Hide Column** hides the column; **Show N Hidden Columns** shows them
  again. The status bar counts the hidden columns.
- **Freeze Up to This Column** keeps the columns up to it in view while
  scrolling sideways; **Unfreeze Columns** releases them.

Hidden columns are left out of copies, exports and Group By; sorts,
filters and searches are not affected, as in the GUI.

## Column types and formats

**Format...** in a column menu chooses how the column's numbers (thousands
separators, decimal places) and dates are shown. Only the display changes:
copies and exports keep the values. See
[Column types and formats](gui.md#column-types-and-formats).

The **Monospace** button shows the cells and the Content pane in a
monospace font, which lines up fixed-width data such as sequences or
codes. The choice is kept in the browser.

## Sheets

Excel workbooks and DuckDB / SQLite databases show one tab per sheet or
table above the grid. SQL results and Group By results are added as
sheets too.

## Running SQL

For DuckDB, SQLite, Parquet and JSON files, the **SQL** button opens the
SQL panel at the bottom.

![The SQL panel](../images/web-sql.png)

Type a query and press **Run** (`Ctrl+Enter`). The result opens as the
sheet **SQL Result**; the next query replaces it, and a file opened again
with other options runs the last query again. `Tab` completes table and
column names, and `Ctrl+↑` / `Ctrl+↓` (or **History...**) bring back
earlier queries. The history is kept in the browser, apart from the GUI's
and the terminal UI's. See [Running SQL](sql.md) for what can be queried.

## Exporting and converting

- **Export** saves the sheet as shown, with its sort and filter applied
  and without the hidden columns, as CSV, TSV, Excel or Parquet. See
  [Export View](exporting.md#export-view).
- **Convert** (text files only) converts the whole file to Parquet or a
  DuckDB database, read as it is shown (its encoding, first row and `##`
  lines) and with each column given the narrowest type its values fit;
  sorts and filters are not applied. See
  [Convert to Parquet / DuckDB](exporting.md#convert-to-parquet--duckdb).

Both run in the background with a row count and a **Stop** button. The
file is written in the browser's memory and then saved by the browser into
its downloads, named after the source file.

## Large text files

The file is read in place, piece by piece, and is never copied whole into
memory. The rows of a text file are kept in memory once read, as in the
GUI, so reading pauses at 100 MiB, 500 MiB, 1 GiB and then every 2 GiB of
rows: **Continue reading** goes on, **Stop here** ends the table at the
rows read. A browser gives a page much less memory than a program gets,
at most 4 GiB, so very large text files are better converted to Parquet
with the GUI or the terminal UI first; Parquet files, databases and Excel
workbooks are read as needed.

## Differences from the GUI

The web app has the GUI's features except these:

- No editing of cells, and no **Follow End of File** or **Auto Revert**:
  the browser cannot write the file, nor see it change.
- One file per page, opened from the page: no recent files, no
  command-line options, no **Show File in File Browser**.
- The SQL history, the monospace choice and the sidebar's width are kept
  in the browser, apart from the GUI's.
- Searching is in the toolbar, not in the sidebar.

## Keyboard shortcuts

| Key                         | Action                                                    |
| --------------------------- | --------------------------------------------------------- |
| `Ctrl+O`                    | Open a file                                               |
| Arrows, `Page Up` / `Page Down`, `Home` / `End` | Move the cursor                       |
| `Ctrl+Home` / `Ctrl+End`    | First / last row                                          |
| `Shift` + a movement key    | Extend the selection                                      |
| `Ctrl+A`                    | Select everything                                         |
| `Ctrl+C`                    | Copy the selection as tab-separated text                  |
| `Enter`, double click       | Show the whole cell in the sidebar                        |
| `Ctrl+G`                    | Go To                                                     |
| `Ctrl+F`                    | Go to the search box                                      |
| `Enter` / `Shift+Enter`     | In the search box: next / previous hit                    |
| `Ctrl+Shift+F`              | The Filter pane                                           |
| `Enter`                     | In a filter value: apply the filter                       |
| `Ctrl+Enter`                | In the SQL panel: run the query                           |
| `Tab`                       | In the SQL panel: complete a name                         |
| `Ctrl+↑` / `Ctrl+↓`         | In the SQL panel: earlier / later queries                 |
| `Escape`                    | Close a dialog or a menu; leave the search box            |
