# Keyboard Shortcuts

[← Contents](README.md)

## GUI

On macOS, use `Cmd` instead of `Ctrl`.

| Shortcut        | Action                                   |
| --------------- | ---------------------------------------- |
| `Ctrl+O`        | Open a file                              |
| `Ctrl+W`        | Close the window                         |
| `Ctrl+Q`        | Quit                                     |
| `Ctrl+Shift+M`  | Edit mode on / off                       |
| `Ctrl+S`        | Save (edit mode)                         |
| `Ctrl+Shift+S`  | Save As (edit mode)                      |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo (edit mode)        |
| `Ctrl+V`        | Paste tab-separated text (edit mode)     |
| `F2`, `Enter`, typing | Edit the current cell (edit mode)  |
| `Delete`, `Backspace` | Clear the selected cells (edit mode) |
| `Ctrl+Shift+E`  | Export the view                          |
| `Ctrl+C`        | Copy the selection as tab-separated text |
| `Ctrl+F`        | Find (Search pane)                       |
| `Ctrl+G`        | Go to a row, cell or column              |
| `Ctrl+Shift+F`  | Filter pane                              |
| `Ctrl+Shift+I`  | Column statistics                        |
| `Ctrl+Shift+H`  | Histogram                                |
| `Ctrl+Shift+P`  | Scatter plot                             |
| `Ctrl+Shift+G`  | Group By                                 |
| `Ctrl+Shift+T`  | Follow the end of the file on / off      |
| `Ctrl+E`        | Run SQL                                  |
| `F1`            | User manual                              |
| Arrow keys, `Page Up` / `Page Down`, `Home` / `End` | Move the current cell |
| `Shift`+click   | Extend the selection                     |
| Double-click    | Show the whole cell in the Content pane (edit mode: edit it) |
| Right-click     | Cell, row or column menu                 |

In the SQL window:

| Shortcut               | Action                              |
| ---------------------- | ----------------------------------- |
| `Ctrl+Enter`           | Run                                 |
| `Ctrl+Up` / `Ctrl+Down`| Previous / next SQL in the history  |
| `Tab`                  | Complete a table, column or keyword |

## Terminal UI

Press `?` or `F1` in the terminal UI to show this list.

### Moving and selecting

| Key                                  | Action                                         |
| ------------------------------------ | ---------------------------------------------- |
| Arrows, `h` `j` `k` `l`              | Move the cursor                                |
| `Shift`+arrows                       | Extend the selection                           |
| `v`                                  | Start / stop selecting with cursor moves       |
| `Ctrl+A`                             | Select all                                     |
| `Esc`                                | Clear the selection                            |
| `PgUp` / `PgDn`, `Ctrl+B` / `Ctrl+F` | Move one page                                  |
| `g` / `G`, `Ctrl+Up` / `Ctrl+Down`   | First / last row                               |
| `Home` / `End`, `0` / `$`            | First / last column                            |
| `Ctrl+G`                             | Go to a row (`120`), a cell (`B12`, `R12C2`) or a column (name or letter) |
| `Tab` / `Shift+Tab`, `]` / `[`       | Next / previous sheet                          |

### Viewing

| Key                  | Action                                                        |
| -------------------- | ------------------------------------------------------------- |
| `Enter`, double click | Show the whole cell in the sidebar (edit mode: edit it)      |
| `S`                  | Statistics of the cursor's column in the sidebar (again: the cell) |
| `B`                  | Histogram of the cursor's column (`Enter`: filter to a bar; `Esc`: back) |
| `M`                  | 2D density of the two selected columns, else of number or date columns near the cursor (`Enter`: filter to a cell; `Esc`: back) |
| `A`                  | Group By: count and aggregate the rows by the values of columns, into a sheet |
| `%`                  | Format the column (or the selected ones): thousands separator, decimals, date format |
| `s`                  | Show / hide the sidebar                                       |
| `<` / `>`            | Narrow / widen the column                                     |
| `x` / `X`            | Hide the column (or the selected ones) / show all columns     |
| `\|`                  | Freeze the columns up to the cursor's (again: unfreeze)      |

### Sorting, filtering and searching

| Key        | Action                                            |
| ---------- | ------------------------------------------------- |
| `o`        | Sort by the column: ascending, descending, none   |
| `f`        | Edit the filter (conditions on columns)           |
| `=` / `!`  | Filter: equal / not equal to the cell             |
| `F`        | Clear the filter                                  |
| `/`        | Search the sheet (empty: clear the search)        |
| `Ctrl+R`   | In the `/` prompt: toggle regular expression search |
| `n` / `N`  | Next / previous search hit                        |

### Data and files

| Key        | Action                                                                     |
| ---------- | -------------------------------------------------------------------------- |
| `y`, `c`   | Copy the selection (tab-separated, through OSC 52)                          |
| `:`        | Run SQL (DuckDB / SQLite / Parquet / JSON); `Up` / `Down`: history, `Tab`: complete |
| `w`        | Export the sheet as shown to CSV / TSV / Excel / Parquet                    |
| `P` / `D`  | Convert to Parquet / DuckDB next to the file and open it (`Esc`: cancel)    |
| `H`        | First row: switch between the guess and header / data (CSV / TSV / Excel) |
| `#`        | Skip lines starting with `##` (on by default for VCF)                       |
| `E`        | Next text encoding: UTF-8, UTF-16LE, UTF-16BE, Shift_JIS (CP932)            |
| `r`        | Revert (reload the file)                                                    |
| `R`        | Toggle Auto Revert                                                          |
| `T`        | Follow the end of the file as it grows (`tail -f`) on / off                 |
| `e`        | Show the file in the file browser                                           |
| `?`, `F1`  | Help                                                                        |
| `q`, `Ctrl+C` | Quit (asks about unsaved changes; `Ctrl+C` again: quit without saving)   |

### Editing

| Key                    | Action                                                         |
| ---------------------- | -------------------------------------------------------------- |
| `I`                    | Edit mode on / off (CSV / TSV / Excel)                         |
| `Enter`, `i`, `F2`     | Edit the cell (edit mode)                                      |
| `Enter` / `Tab` / `Esc` | While editing: keep and move down / keep and move right / cancel |
| `Delete`, `Backspace`  | Clear the selected cells (edit mode)                           |
| Paste                  | Paste tab-separated text into the selected cells (edit mode)   |
| `u` / `Ctrl+R`         | Undo / redo (edit mode)                                        |
| `Ctrl+S`               | Save (edit mode)                                               |

### Filter dialog

| Key        | Action                                            |
| ---------- | ------------------------------------------------- |
| `↑` / `↓`  | Move between lines                                |
| `Tab`      | Next field: column, operator, value               |
| `←` / `→`  | Change the column, operator, or All / Any         |
| `Ctrl+N`   | Add a condition                                   |
| `Ctrl+D`   | Delete the condition                              |
| `Ctrl+U`   | Clear the value                                   |
| `Enter`    | Apply                                             |
| `Esc`      | Cancel                                            |

### Histogram

| Key                  | Action                                    |
| -------------------- | ----------------------------------------- |
| `↑` / `↓`, `k` / `j` | Select a bar                              |
| `←` / `→`, `h` / `l` | Previous / next column                    |
| `Enter`              | Filter the sheet to the selected bar      |
| `F`                  | Clear the filter                          |
| `Esc`, `B`, `q`      | Back to the sheet                         |

### 2D density

| Key                  | Action                                    |
| -------------------- | ----------------------------------------- |
| Arrows, `h j k l`    | Select a cell                             |
| `x` / `X`            | Next / previous column on X               |
| `y` / `Y`            | Next / previous column on Y               |
| `t`                  | Swap X and Y                              |
| `L`                  | Logarithmic / linear colour scale         |
| `Enter`              | Filter the sheet to the selected cell     |
| `F`                  | Clear the filter                          |
| `b`                  | Text on X: box plots / density            |
| `Esc`, `M`, `q`      | Back to the sheet                         |

### Group By dialog

| Key                  | Action                                    |
| -------------------- | ----------------------------------------- |
| `↑` / `↓`, `k` / `j` | Move between lines                        |
| `g`, `Space`         | Group by the column on / off              |
| `a`                  | Aggregate the column on / off             |
| `←` / `→`, `Space`   | Last line: choose a function, on / off    |
| `Enter`              | Group                                     |
| `Esc`                | Cancel                                    |

### Format dialog

| Key                  | Action                                    |
| -------------------- | ----------------------------------------- |
| `↑` / `↓`            | Move between lines                        |
| `←` / `→`, `Space`   | Change the setting                        |
| `Delete`             | Reset the format                          |
| `Enter`              | Apply                                     |
| `Esc`                | Cancel                                    |
