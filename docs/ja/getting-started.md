# はじめに

[← 目次](README.md)

## インストール

TableSkim は必要なライブラリをすべて組み込んだネイティブパッケージとして配布しているので、ほかに何もインストールする必要はありません。

| プラットフォーム | パッケージ                                                  |
| ---------------- | ----------------------------------------------------------- |
| Linux            | `.deb`（Ubuntu 24.04 以降）、`.rpm`（RHEL 10 とその互換ディストリビューション）、またはどのディストリビューションでも動く `.zip` |
| macOS            | `.dmg` ディスクイメージ、または `TableSkim.app` を含む `.zip` |
| Windows          | インストーラー（`.exe`）、`.zip`                            |

`.deb` と `.rpm` は Ubuntu 24.04 と Rocky Linux 10 でビルドしているため、それと同じかより新しいディストリビューションにしかインストールできません。ほかのディストリビューションや古いもの（glibc 2.28 以降：RHEL 8、Debian 10、Ubuntu 20.04 以降）では Linux 用の ZIP を使ってください。インストールは不要で、好きな場所に展開して中の `bin/tableskim` を実行します。

| ZIP                                        | 内容                                                  |
| ------------------------------------------ | ----------------------------------------------------- |
| `tableskim-<version>-Linux-x86_64.zip`     | GUI（Wayland と X11）とターミナル UI                  |
| `tableskim-<version>-Linux-x11-x86_64.zip` | X11 専用の GUI（Wayland のライブラリが不要）とターミナル UI |
| `tableskim-tui-<version>-Linux-x86_64.zip` | ターミナル UI のみ（デスクトップのないサーバー向け）  |

macOS 版は開発元の証明書で署名していないため、初回は macOS が許可を求めます。`TableSkim.app` を Control キーを押しながらクリック(右クリック)して **開く** を選ぶか、**システム設定 → プライバシーとセキュリティ** で許可してください。「壊れている」と表示されるときは、ターミナルでダウンロードの印を外します。

```sh
xattr -dr com.apple.quarantine /Applications/TableSkim.app
```

自分でビルドする場合は [README](../../README.md#building) の *Building*
の節を参照してください。

### 各プラットフォームでの `tableskim` コマンド

- **Linux**：`.deb` と `.rpm` は `tableskim` コマンドとデスクトップエントリをインストールするので、アプリケーションメニューにも TableSkim が表示されます。ZIP ではコマンドは `bin/` にあります。どこからでも使うには `PATH` の通った場所にリンクを作ってください。
- **macOS**：コマンドはアプリバンドル内の実行ファイルです。ターミナルから使うには、`PATH` の通った場所にリンクを作ります。

  ```sh
  ln -s /Applications/TableSkim.app/Contents/MacOS/TableSkim /usr/local/bin/tableskim
  ```

- **Windows**：`TableSkim.exe` は GUI アプリケーションです（コンソールは開きません）。ターミナル UI には、同じ場所にインストールされる
  `tableskim-tui.exe` を使います。これは起動したターミナルの中で動きます。

### このマニュアル

このマニュアルは TableSkim に同梱されています。GUI では **Help → User Manual**（`F1`）で開けます。また、Web ブラウザで読める HTML 版もインストールされるので、ターミナル UI を使うときに便利です。

| プラットフォーム | 場所                                                                     |
| ---------------- | ------------------------------------------------------------------------ |
| Linux            | `/usr/share/doc/tableskim/html/README.html`（ZIP では `share/doc/tableskim/html/README.html`） |
| macOS            | `.dmg` または `.zip` 内の、`TableSkim.app` の隣にある `Manual` フォルダ |
| Windows          | インストール先フォルダの `Manual\README.html`。インストーラーはスタートメニューに **TableSkim User Manual** も追加します |

## TableSkim の起動

```sh
tableskim [オプション] [ファイル...]
```

- グラフィカルな画面が使えるときは **GUI** が開きます。コマンドラインで指定したファイルは、それぞれ別のウィンドウで開きます。ファイルを指定しないと、**Open...** ボタンだけの空のウィンドウが開きます。
- グラフィカルな画面が使えないときは **ターミナル UI** が起動します。
  Linux では `DISPLAY` も `WAYLAND_DISPLAY` も設定されていないとき、macOS
  では SSH 接続中のときです。`-t` / `--tui` を付けると、画面が使える場合でもターミナル UI で起動します。
- ターミナル UI で表示できるファイルは 1 つだけで、コマンドラインで指定する必要があります。

GUI では **File → Open...**（`Ctrl+O`）からもファイルを開けます。macOS では、アプリケーションのアイコンにファイルをドロップするか、Finder の「このアプリケーションで開く」を使うこともできます。

```sh
tableskim sales.xlsx               # GUI（画面がなければ TUI）
tableskim a.csv b.parquet          # GUI のウィンドウを 2 つ開く
tableskim -t data.csv.gz           # ターミナル UI
tableskim --encoding cp932 old.csv # Shift_JIS の CSV ファイルを読む
```

## コマンドラインオプション

| オプション          | 説明                                                                |
| ------------------- | ------------------------------------------------------------------- |
| `-t`, `--tui`       | グラフィカルな画面が使えてもターミナル UI を使う                    |
| `--encoding <名前>` | CSV / TSV ファイルの文字コード：`auto`（既定）、`utf-8`、`utf-16le`、`utf-16be`、`cp932`（`shift_jis`） |
| `--log <ファイル>`  | ログメッセージをファイルに書き出す                                  |
| `-v`, `--verbose`   | 詳細（デバッグ）ログを有効にする                                    |
| `-V`, `--version`   | バージョン情報を表示して終了する                                    |
| `-h`, `--help`      | オプションの一覧を表示して終了する                                  |

ターミナル UI は、ログメッセージが画面に重なって表示されないよう、`--log`
を指定したときだけログを書き出します。

## 最初の画面

ウィンドウには表が表示され、左に行番号、上に列見出しが並びます。行は読み込まれたそばから表示されるので、大きなファイルの読み込み中でもスクロール、並べ替え、検索ができます。

![GUI で CSV ファイルを開いたところ](../images/gui-main-window.png)

CSV / TSV や Excel ファイルの先頭行は、既定ではデータとして表示され、列名は表計算ソフトと同じく `A`、`B`、`C`… になります。先頭行が列名の場合は
**View → Use first row as header**（ターミナル UI では `H`）をオンにしてください。Parquet ファイルとデータベースは、常にファイル自身の列名を使います。

続きは [GUI の使い方](gui.md) または [ターミナル UI の使い方](tui.md) を参照してください。
