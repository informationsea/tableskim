# Sorting, Filtering, Searching and Statistics

[← Contents](README.md)

This page describes how TableSkim compares values. The rules are the same
in the GUI and the terminal UI, and for every file format.

## Values and types

Each cell holds one of: empty (NULL), text, an integer, a decimal number, a
boolean (TRUE / FALSE), a date or a date-time. Excel, Parquet, JSON and
database files store these types. In CSV / TSV files every cell is text,
but TableSkim still recognizes:

- **numbers**: text such as `42` or `-3.5`;
- **dates and date-times** in ISO format: `2024-01-31`,
  `2024-01-31 12:34`, `2024-01-31T12:34:56.789`;
- **booleans**: `TRUE` / `FALSE`, `yes` / `no`, `1` / `0` (in any case),
  when compared with a boolean.

## Sorting

- Sorting is by one column at a time, ascending or descending; the third
  click (GUI) or `o` (TUI) restores the file order.
- **Empty cells always come last**, in both directions.
- Numbers (including numeric text, booleans and dates) come before text in
  ascending order and are compared numerically, so `9` comes before `10`.
- Text is compared byte by byte (in UTF-8), which puts upper-case letters
  before lower-case ones.
- Rows with equal values keep their order in the file.

For CSV / TSV and Excel files, sorting reads the whole sheet into memory
first. For Parquet, JSON and databases, the sort runs inside the database,
so it works for tables of any size.

## Filtering

A filter is a list of **conditions**, combined with **All** (AND: every
condition must match) or **Any** (OR: at least one must match). Each
condition tests one column.

### Operators

| Operator            | Matches cells that...                                             |
| ------------------- | ----------------------------------------------------------------- |
| `contains`          | contain the text, ignoring upper and lower case                   |
| `not contains`      | do not contain the text, ignoring case                            |
| `=`                 | are equal to the value (see below)                                |
| `≠`                 | are not equal to the value                                        |
| `<` `≤` `>` `≥`     | are less than / at most / greater than / at least the value       |
| `is empty`          | are empty (NULL or empty text)                                     |
| `is not empty`      | are not empty                                                     |
| `matches`           | contain a match of the regular expression, ignoring case          |
| `not matches`       | contain no match of the regular expression                        |
| `is a number`       | are numbers or numeric text (not booleans, dates, NaN or infinity) |
| `is a date`         | are dates, date-times or ISO date text that is not a number       |

### How `=`, `≠`, `<`, `≤`, `>` and `≥` compare

The comparison depends on the cell and on the value you type:

- **Numbers**: if both the cell and the value are numbers (numeric text
  included), they are compared numerically. `Age ≥ 45` keeps 45, 50 and
  `"60"`. A cell that is not a number is compared as text (see below), so
  `"abc"` matches too, as it sorts after `"45"`; add `is a number` to keep
  numbers only.
- **Dates**: a date or date-time cell (or ISO date text) compared with a
  date or date-time value is compared in time. A **date value means the
  whole day**, so `= 2024-01-31` matches `2024-01-31 08:00` and
  `2024-01-31 23:59`, and `< 2024-02-01` matches everything before that day.
- **Booleans**: a boolean cell compared with `TRUE` / `FALSE`, `yes` / `no`
  or `1` / `0`.
- **Text**: anything else is compared as text, byte by byte, so `=` is
  case-sensitive for text. Use `contains` or `matches` to ignore case.

### Regular expressions

`matches` and `not matches` use [RE2 syntax](https://github.com/google/re2/wiki/Syntax),
ignoring upper and lower case. The pattern may match anywhere in the cell;
anchor it with `^` and `$` to match the whole cell, for example
`^(Tokyo|Osaka)$`. An invalid pattern is reported and the filter is not
applied.

### Examples

| Goal                                         | Condition(s)                                 |
| -------------------------------------------- | -------------------------------------------- |
| People in Tokyo aged 45 or older             | `City = Tokyo` **and** `Age ≥ 45` (Match: All) |
| Rows from January 2024                       | `Date ≥ 2024-01-01` **and** `Date < 2024-02-01` |
| Rows on a given day, whatever the time       | `Timestamp = 2024-01-31`                     |
| E-mail addresses at example.com or .org      | `Email matches @example\.(com\|org)$`         |
| Rows with a missing value                    | `Salary is empty`                            |
| Tokyo or Osaka                               | `City = Tokyo`, `City = Osaka` (Match: Any)  |

### Quick filters

The quick filters (`Filter: = "value"` in the GUI cell menu, `=` / `!` in
the TUI) add an `=` / `≠` condition with the value of the cell as it is
shown, which is then compared by type as described above. On an empty
cell they add `is empty` / `is not empty`. A condition that is already in
the filter is not added twice.

## Searching

- A search looks for text in **every cell** of the current sheet, in the
  current sort order and filter.
- It ignores upper and lower case and matches anywhere in the cell text, as
  the cell is displayed.
- With **Regex** (GUI) or `Ctrl+R` (TUI), the text is an RE2 regular
  expression.
- Hits are highlighted. The search runs in the background, so you can keep
  working while it scans a large file.

## Column statistics

Statistics are computed for one column over the rows of the current view
(after the filter):

| Item         | Meaning                                                       |
| ------------ | ------------------------------------------------------------- |
| Rows         | Number of rows                                                |
| Empty        | Number of empty cells (NULL or empty text), with percentage   |
| Distinct     | Number of different non-empty values                          |
| Numbers      | Number of cells that are numbers (numeric text included)      |
| Min / Max    | Smallest and largest value, in sort order                     |
| Sum / Mean   | Sum and mean of the numbers                                   |
| Std dev      | Standard deviation of the numbers                             |
| Most frequent | The most common values, with counts and percentages          |

Changing the filter recomputes the statistics. For Parquet, JSON and
DuckDB files the values are counted inside the database.

## Histograms

A histogram counts one column over the rows of the current view (after the
filter), like the column statistics. It is opened with **Edit →
Histogram...** in the GUI (see [Using the GUI](gui.md#histograms)) and `B`
in the terminal UI (see [Using the Terminal UI](tui.md#histograms)).

### What is counted

The values of the column are read as the filters read them:

- **numbers**: integers, decimals and numeric text;
- **times**: dates, date-times and ISO date text (`2024-01-31`,
  `2024-01-31 12:34`).

If the column holds both, the histogram counts the kind it holds more of;
the values of the other kind are reported as "other values" in the
summary, and empty cells as "empty". A column with neither numbers nor
dates shows its **most frequent values** instead.

### How bins are chosen

The aim is about **30 bins** with widths that are easy to read:

- **Numbers**: the width is 1, 2 or 5 times a power of ten (…, 0.1, 0.2,
  0.5, 1, 2, 5, 10, 20, 50, …), and the bins start at a multiple of the
  width. Integer columns use widths of at least 1, and get **one bin per
  value** when their values span at most 30 numbers.
- **Times**: whole seconds, minutes, hours, days, weeks (starting on
  Monday), months or years, whichever gives about 30 bins. Months and years
  follow the calendar, so a month bin really is one month long.

Each bin includes its lower edge and excludes its upper edge, written
`[lower, upper)`. The last bin of numbers also includes its upper edge,
written `[lower, upper]`, so the maximum value is counted.

### Filtering from a bar

Choosing a bar (double-click in the GUI, `Enter` in the TUI) adds three
conditions to the filter, `is a number` (`is a date` for times), `≥ lower`
and `< upper` (`≤ upper` for the last bin of numbers), which keep exactly
the rows counted in that bar. The first one leaves out the "other values":
without it, text such as `15 kg` would still pass `≥ 10` and `< 20` as
text. For a
bin of years, months or days the edges are dates, such as
`≥ 1994-01-01` and `< 1996-01-01`. For a text column, the bar's value is added as an `=` condition. You can see and
edit these conditions in the Filter pane (`f` in the TUI).

For Parquet, JSON and DuckDB files the values are counted inside the
database.

## Scatter plots and 2D density

A scatter plot draws one column (X) against another (Y) over the rows of
the current view; its 2D density counts those rows on a grid. They are
opened with **Edit → Scatter Plot...** in the GUI (see [Using the
GUI](gui.md#scatter-plots-and-2d-density)) and, as a density only, `M` in
the terminal UI (see [Using the Terminal UI](tui.md#2d-density)).

- Each column is read as for a [histogram](#what-is-counted): numbers or
  times, whichever it holds more of. A row is plotted only if both of its
  values are of the column's kind; the other rows are reported as
  "skipped".
- The grid's cells are the bins of a histogram of each column, chosen as
  [above](#how-bins-are-chosen) but about 50 per axis in the GUI and as
  many as the terminal fits in the TUI.
- The density counts every row. The points of the GUI's scatter plot are a
  random sample of at most **200,000** rows, the same sample each time.
- **r** is Pearson's correlation coefficient of the rows plotted, **R²** its
  square, and the line `y = a x + b` the least-squares fit of Y on X. Dates
  are taken as points in time, so *r* works for them too, but the line's
  equation is shown for numbers only. *r* is undefined when a column has a
  single value.

When X holds text, its 30 most frequent values (in the order of the
histogram's most frequent values) are categories, and the other values
together are **(other)**. Each category gets a box plot of its Y values:

- the quartiles by linear interpolation between the closest values, as
  spreadsheets' `QUARTILE.INC` computes them (Q1 of 1, 2, …, 8 is 2.75);
- the whiskers end at the furthest values within 1.5 interquartile ranges
  (Q3 − Q1) of the box; the values beyond are outliers;
- the mean of the values.

Choosing a box (double-click in the GUI, `Enter` in the TUI) adds `=` the
category's value on X and `is a number` (or `is a date`) on Y. "(other)"
cannot be chosen, as it is not one value.

Choosing a cell (double-click in the GUI, `Enter` in the TUI) adds the
conditions of both columns' bins to the filter, which keep exactly the rows
counted in the cell. Dragging a rectangle in the GUI adds `is a number` (or
`is a date`), `≥` and `≤` conditions on both columns, with the bounds
rounded a little finer than the bins.

## Group By

**Edit → Group By...** in the GUI (see [Using the GUI](gui.md#group-by))
and `A` in the terminal UI (see [Using the Terminal UI](tui.md#group-by))
count the rows of the current view by the values of some columns.

- Rows whose values are equal in every group column form a group. Values
  are equal only when they are the same value: `1`, `01` and `1.0` as text
  are three groups, although they sort alike. Empty cells (and the missing
  cells of short rows) form one group, shown last.
- Groups are listed in the sort order of their values, the first group
  column first, as [sorting](#sorting) orders them.
- The aggregates follow the [column statistics](#column-statistics):
  **count** and **distinct** count the non-empty values; **sum**, **mean**
  and **std dev** (the sample standard deviation) use the values that are
  numbers, including numeric text; **min** and **max** are the first and
  last values in sort order, so numbers come before text.
- A group column whose values are all different would make one group per
  row; more than 1,000,000 groups stop the grouping with an error.
