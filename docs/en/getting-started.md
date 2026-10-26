# Getting Started

[← Contents](README.md)

## Installing

TableSkim is distributed as native packages with all libraries built in, so
nothing else needs to be installed.

| Platform | Packages                                                               |
| -------- | ---------------------------------------------------------------------- |
| Linux    | `.deb` (Ubuntu 24.04 or later), `.rpm` (RHEL 10 and its rebuilds), or a `.zip` for any distribution |
| macOS    | `.dmg` disk image or `.zip` containing `TableSkim.app`                 |
| Windows  | Installer (`.exe`) or `.zip`                                           |

The `.deb` and `.rpm` packages are built on Ubuntu 24.04 and Rocky Linux 10
and install only on distributions at least as new. For other or older
distributions (glibc 2.28 or later: RHEL 8, Debian 10, Ubuntu 20.04 and
newer), use one of the Linux ZIPs, which need no installation: unpack one
anywhere and run `bin/tableskim` in it.

| ZIP                                     | Contents                                              |
| --------------------------------------- | ----------------------------------------------------- |
| `tableskim-<version>-Linux-x86_64.zip`     | GUI (Wayland and X11) and terminal UI               |
| `tableskim-<version>-Linux-x11-x86_64.zip` | GUI for X11 only, which needs no Wayland libraries, and terminal UI |
| `tableskim-tui-<version>-Linux-x86_64.zip` | Terminal UI only, for servers without a desktop     |

The macOS app is not signed with a developer certificate, so macOS asks for
your permission the first time: Control-click (right-click) `TableSkim.app`
and choose **Open**, or allow it under **System Settings → Privacy &
Security**. If macOS says the app is damaged, remove the download mark in a
terminal:

```sh
xattr -dr com.apple.quarantine /Applications/TableSkim.app
```

To build TableSkim yourself, see the *Building* section of the
[README](../../README.md#building).

### The `tableskim` command on each platform

- **Linux**: the `.deb` and `.rpm` packages install the `tableskim` command
  and a desktop entry, so TableSkim also appears in the application menu.
  A ZIP has the command in `bin/`; link it into your `PATH` to use it from
  anywhere.
- **macOS**: the command is the executable inside the app bundle. To use
  it from a terminal, link it into your `PATH`:

  ```sh
  ln -s /Applications/TableSkim.app/Contents/MacOS/TableSkim /usr/local/bin/tableskim
  ```

- **Windows**: `TableSkim.exe` is the GUI application (it does not open a
  console). For the terminal UI use `tableskim-tui.exe`, which is installed
  next to it and runs in the terminal it is started from.

### This manual

The manual comes with TableSkim. In the GUI, open it with **Help → User
Manual** (`F1`). It is also installed as HTML pages for a web browser,
which is handy when you use the terminal UI:

| Platform | Location                                                                 |
| -------- | ------------------------------------------------------------------------ |
| Linux    | `/usr/share/doc/tableskim/html/README.html` (ZIP: `share/doc/tableskim/html/README.html`) |
| macOS    | The `Manual` folder next to `TableSkim.app` in the `.dmg` or `.zip`      |
| Windows  | `Manual\README.html` in the installation folder; the installer also adds **TableSkim User Manual** to the Start menu |

## Starting TableSkim

```sh
tableskim [options] [files...]
```

- With a graphical display, `tableskim` opens the **GUI**. Each file given
  on the command line opens in its own window. Without a file, an empty
  window with an **Open...** button appears.
- Without a graphical display, `tableskim` starts the **terminal UI**: on
  Linux when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set, and on macOS in
  an SSH session. `-t` / `--tui` starts the terminal UI even when a display
  is available.
- The terminal UI shows exactly one file, which must be given on the
  command line.

You can also open files from the GUI with **File → Open...** (`Ctrl+O`), and
on macOS by dropping files on the application icon or using *Open With* in
the Finder.

```sh
tableskim sales.xlsx               # GUI (or TUI without a display)
tableskim a.csv b.parquet          # two GUI windows
tableskim -t data.csv.gz           # terminal UI
tableskim --encoding cp932 old.csv # read a Shift_JIS CSV file
```

## Command-line options

| Option              | Description                                                         |
| ------------------- | ------------------------------------------------------------------- |
| `-t`, `--tui`       | Use the terminal UI even if a graphical display is available        |
| `--encoding <name>` | Encoding of CSV / TSV files: `auto` (default), `utf-8`, `utf-16le`, `utf-16be` or `cp932` (`shift_jis`) |
| `--log <file>`      | Write log messages to a file                                        |
| `-v`, `--verbose`   | Enable verbose (debug) logging                                      |
| `-V`, `--version`   | Show version information and exit                                   |
| `-h`, `--help`      | Show the list of options and exit                                   |

The terminal UI writes log messages only with `--log`, because they would
otherwise be drawn over the screen.

## A first look

The window shows the table with row numbers on the left and column headers
on top. Rows appear as soon as they are read; you can scroll, sort and
search while a large file is still loading.

![A CSV file in the GUI](../images/gui-main-window.png)

By default the first row of a CSV / TSV or Excel file is shown as data, and
the columns are named `A`, `B`, `C`, ... like a spreadsheet. If the first row
holds column names, turn on **View → Use first row as header** (`H` in the
terminal UI). Parquet files and databases always use their own column names.

Continue with [Using the GUI](gui.md) or [Using the Terminal UI](tui.md).
