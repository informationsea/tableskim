# Editing Cells

[← Contents](README.md)

TableSkim opens files for viewing, and nothing you do changes a file unless
you turn on **edit mode**. In edit mode you can change the values of cells
and save the file again.

| Format                                             | Editing |
| -------------------------------------------------- | ------- |
| CSV, TSV, VCF and other text files (compressed too) | Yes     |
| Excel (`.xlsx`, `.xlsm`)                           | Yes     |
| Parquet, JSON, DuckDB, SQLite, SQL results         | No      |

Only cell values change: rows and columns cannot be added or removed.

## Edit mode

- GUI: **Edit → Edit Mode** (`Ctrl+Shift+M`). The window title shows
  `[Editing]`.
- Terminal UI: `I`. The status bar shows `EDIT`.

Edit mode applies to every sheet of the file. It stays on when the file is
saved or reloaded, and it is off when another file is opened. Turning it
off with unsaved changes asks whether to save them first.

## Changing cells

In the GUI:

- `F2`, `Enter` or a double click edits the current cell; typing replaces
  its value with what you type.
- In the cell, `Enter` keeps the value and moves down, `Tab` moves right
  (`Shift` moves up / left), and `Esc` cancels. `Alt+Enter` starts a new
  line in the cell. Clicking elsewhere keeps the value.
- `Delete` or `Backspace` clears the selected cells.
- **Edit → Paste** (`Ctrl+V`) pastes tab-separated text, such as cells
  copied from TableSkim or a spreadsheet, from the selected cell on. A
  single value fills the whole selection.
- **Edit → Undo** (`Ctrl+Z`) and **Edit → Redo** (`Ctrl+Shift+Z`).

A cell of a frozen column is edited in a small dialog.

In the terminal UI:

- `Enter`, `i`, `F2` or a double click edits the cell under the cursor in
  the status line. `Enter` keeps the value and moves down, `Tab` moves
  right, `Esc` cancels, and `Ctrl+U` clears the text.
- `Delete` or `Backspace` clears the selected cells.
- Pasting into the terminal pastes tab-separated text into the selected
  cells.
- `u` undoes and `Ctrl+R` redoes.

Outside edit mode these keys keep their usual meaning (for example `Enter`
shows the cell in the sidebar).

Edited cells are shown in green until the file is saved, and the title
(GUI) or status bar (terminal UI) starts with `*` while there are unsaved
changes. Undo and Redo work for each sheet separately.

### What a cell becomes

- **Text files**: every cell is text, kept exactly as typed.
- **Excel**: the cell keeps its type when the text reads as one. A number
  cell (or an empty one) takes a number, a date or date-time cell an ISO
  date such as `2024-03-15` or `2024-03-15 09:30`, a boolean cell `TRUE` or
  `FALSE`. Anything else becomes text, and a text cell stays text, so
  `007` keeps its zeros. Empty text clears the cell.

Some cells cannot be edited: Excel formulas, error values and times shown
as text (`hh:mm:ss`, `[h]:mm:ss`), cells covered by a merged cell (except
its top-left cell), the header row, and rows that have not been read yet.

### Sorting and filtering

An edited row stays where it is: the sort and filter are applied again only
when you change them, as in a spreadsheet. Searches, column statistics and
histograms start over with the new values.

## Saving

- GUI: **File → Save** (`Ctrl+S`), or **File → Save As...**
  (`Ctrl+Shift+S`) to save a copy in the same format and open it.
- Terminal UI: `Ctrl+S`.

Saving keeps everything you did not change:

- **Text files**: only the edited lines are written anew. All other lines
  keep their bytes: quotes, line breaks (LF or CRLF), lines starting with
  `##`, the text encoding (UTF-8, UTF-16 or Shift_JIS), a byte order mark,
  and the compression (gzip, bgzip, bzip2, xz or zstd). An edited line is
  written with the same delimiter, quoting values only where needed. A
  character that the encoding cannot hold (for example an emoji in a
  Shift_JIS file) stops the save with a message.
- **Excel**: only the edited cells change; other sheets, formats, formulas
  and merged cells stay. Formulas are computed again when Excel opens the
  file.

The new file is written next to the original and replaces it only when it
is complete, so a failed save leaves the file as it was. The file keeps its
permissions, and a symbolic link keeps pointing to it. After saving, the
file is opened again (the position, sort and filter stay), and Undo starts
over.

If another program has changed the file since TableSkim opened it, saving
fails; use Save As, or reload the file (which drops your changes).

A bgzip file is written in bgzip blocks again, but an index made for it
(`.tbi`, `.csi`) no longer matches and must be made again.

## Unsaved changes

TableSkim asks before unsaved changes would be lost: when the window is
closed, when quitting, when reloading the file (Revert), and when changing
the header, `##` or encoding option, which read the file again.

- GUI: **Save**, **Don't Save** or **Cancel**.
- Terminal UI: `y` saves, `n` discards, `Esc` cancels. Pressing `Ctrl+C`
  again quits without saving.

While there are unsaved changes, Auto Revert does not reload the file. If
the file changes on disk meanwhile, the title or status bar says
`(changed on disk)`.

**Export View** writes the sheet with the unsaved changes; **Convert to
Parquet / DuckDB** reads the file and so converts it as it was last saved.
See [Exporting and Converting](exporting.md).
