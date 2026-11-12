# Changelog

What changes in TableSkim for Visual Studio Code from release to release.
Its version is TableSkim's; the changes of TableSkim itself (formats,
statistics, charts, SQL) are in its
[release notes](https://github.com/informationsea/tableskim/releases).

## Unreleased

- Opens OpenDocument spreadsheets (`.ods`), by default like Excel
  workbooks.

## v0.3.4

The first release, as a preview.

- Opens CSV, TSV and VCF files (also compressed with gzip, bzip2, xz or
  zstd), Excel workbooks, Parquet and JSON / NDJSON files and DuckDB and
  SQLite databases as tables in a read-only editor. Parquet, Excel, DuckDB
  and SQLite files open in it by default; the others with **Open in
  TableSkim** or **Reopen Editor With…**.
- Sorting, filters, search, statistics, histograms (stacked by another
  column too), scatter plots, Group By, SQL, exports and conversion to
  Parquet or DuckDB, as in TableSkim's web app.
- Chart group filters preserve exact cell text, including leading zeros
  and case, so the rows kept match the group's count.
- Exports and chart images are saved where you choose; the file viewed is
  never written.
- Asks whether to read the file again when it changes on disk;
  **TableSkim: Reload File** reads it again at any time, and so does
  **Reload** if the engine stops.
- Follows the color theme, and works with Remote Development and in
  Restricted Mode.
