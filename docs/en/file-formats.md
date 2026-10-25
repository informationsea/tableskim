# Supported File Formats

[← Contents](README.md)

TableSkim picks the format from the file name and its first bytes. Parquet,
DuckDB and SQLite files are recognized by their first bytes, so they open
even with another extension, and text starting with `{` or `[` is read as
JSON. The extensions below are the usual ones. Every
format opens in the same kind of window; formats with several tables show
each table as a **sheet** (a tab at the bottom of the GUI window, or at the
top of the terminal UI).

| Format                | Extensions                                         | Sheets                    |
| --------------------- | -------------------------------------------------- | ------------------------- |
| CSV                   | `.csv`                                             | one                       |
| TSV                   | `.tsv`, `.tab`                                     | one                       |
| VCF                   | `.vcf`, `.vcf.gz`, `.vcf.bgz`                      | one                       |
| Other text            | `.txt` and unknown extensions that look like text  | one                       |
| Excel                 | `.xlsx`, `.xlsm`                                   | one per worksheet         |
| Parquet               | `.parquet`                                         | one                       |
| JSON                  | `.json`, `.ndjson`, `.jsonl`                       | one                       |
| DuckDB                | `.duckdb`, `.ddb`                                  | one per table and view    |
| SQLite                | `.sqlite`, `.sqlite3`, `.db`, `.db3`               | one per table and view    |

![An Excel workbook with several worksheets](../images/gui-multisheet.png)

## CSV, TSV and other text files

- The delimiter is a comma for `.csv` and a tab for `.tsv`, `.tab` and
  `.vcf`; for other text files it is detected.
- Rows may be **ragged** (have different numbers of fields); missing cells
  are shown empty.
- Quoted fields with commas, quotes and line breaks are supported.
- Numbers, dates and booleans are recognized for sorting, filtering and
  statistics, while the text is shown as written in the file.
- **Use first row as header** (View menu; `H` in the terminal UI) takes the
  column names from the first row. It is off by default.

### Compressed files

CSV / TSV / VCF files compressed with **gzip** (`.gz`, `.bgz`), **bzip2**
(`.bz2`), **xz** (`.xz`) or **zstd** (`.zst`) are decompressed on the fly,
for example `data.csv.gz` or `variants.vcf.bgz`.

### Text encodings

CSV / TSV files may be in **UTF-8** (with or without a BOM), **UTF-16LE /
UTF-16BE** (with or without a BOM) or **Shift_JIS (CP932)**, as written by
Japanese versions of Excel. The encoding is detected from the start of the
file. If the guess is wrong:

- GUI: choose the encoding from **View → Text Encoding**.
- Terminal UI: press `E` to switch to the next encoding.
- Command line: `--encoding utf-8 | utf-16le | utf-16be | cp932`.

### VCF and `##` lines

Lines starting with `##` (such as VCF meta-information lines) can be skipped
with **View → Skip lines starting with ##** (`#` in the terminal UI). This is
on by default for `.vcf` files, so that the `#CHROM ...` line becomes the
first row.

### Large text files and memory

CSV / TSV files have no index, so to sort or filter them TableSkim keeps the
rows it has read in memory. When those rows take about **100 MB**,
**500 MB** and **1 GB**, and then every **2 GB**, TableSkim pauses and asks
whether to continue:

- **Continue** keeps reading the file.
- **Stop Reading** shows only the rows read so far, as if the file ended
  there.

If you need to work with very large text files often, convert them to
Parquet or DuckDB once (see [Exporting and Converting](exporting.md)): those
formats are sorted and filtered inside the database, without loading every
row.

## Excel

- `.xlsx` and `.xlsm` workbooks are supported; every worksheet is a sheet.
- Dates (including workbooks using the 1904 date system), numbers and
  booleans keep their types.
- The value of a merged range is shown in its top-left cell; the other cells
  of the range are empty.
- **Use first row as header** works as for CSV.

## Parquet

Parquet files are read through DuckDB. Sorting and filtering run inside
DuckDB, so they work on files of any size. You can also run SQL on a
Parquet file (see [Running SQL](sql.md)).

## JSON

- An array of objects (`[{...}, {...}]`) or one object per line (NDJSON /
  JSON Lines, `.ndjson`, `.jsonl`).
- gzip- or zstd-compressed JSON (`.json.gz`, `.jsonl.zst`, ...) is
  supported.
- Object keys become columns. Nested objects and arrays are shown as text,
  for example `{'name': user1, 'tags': [a]}` or `[1, 2, 3]`.
- SQL can be run on JSON files.

## DuckDB and SQLite databases

Every table and view of the database is shown as a sheet. Sorting and
filtering run inside the database, so they are fast even for large tables.
In DuckDB, tables come before views and those of the `main` schema first;
other schemas are named `schema.table`. Should two names coincide (table
`b` of schema `a` and a table named `a.b` in `main`), the sheet shows the
first one; use SQL for the other.
Databases are opened read-only, so TableSkim never changes them; you can
still query them with SQL (see [Running SQL](sql.md)).

![A DuckDB database: one sheet per table, and a SQL result sheet](../images/gui-sql-result.png)

## Non-ASCII names

File names, sheet names and cell contents in Japanese and other non-ASCII
scripts are supported on every platform.
