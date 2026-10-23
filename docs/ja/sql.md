# SQL の実行

[← 目次](README.md)

TableSkim は **DuckDB**、**SQLite**、**Parquet**、**JSON** ファイルに SQL を実行できます。クエリの結果は **SQL Result** という追加のシートに表示され、ほかのシートと同じように並べ替え、フィルタ、検索、統計、エクスポートができます。

- GUI：**Edit → Run SQL...**（`Ctrl+E`）で SQL ウィンドウを開きます。
- ターミナル UI：`:` で画面下部に SQL プロンプトを開きます。

![SQL ウィンドウと結果のシート](../images/gui-sql-result.png)

## クエリできるもの

| ファイル              | SQL の方言 | 使えるテーブル                                          |
| --------------------- | ---------- | ------------------------------------------------------- |
| DuckDB データベース   | DuckDB     | データベースのすべてのテーブルとビュー                  |
| SQLite データベース   | SQLite     | データベースのすべてのテーブルとビュー                  |
| Parquet ファイル      | DuckDB     | 拡張子を除いたファイル名（`sales.parquet` → `sales`）   |
| JSON ファイル         | DuckDB     | 拡張子を除いたファイル名（`events.jsonl.gz` → `events`） |

空白、記号、ASCII 以外の文字を含む名前は、`SELECT * FROM "多言語サンプル"`
のようにダブルクォートで囲みます。

## クエリの実行

SQL を入力し、GUI では **Run**（`Ctrl+Enter`）、ターミナル UI では `Enter`
を押します。

```sql
SELECT City, count(*) AS people, round(avg(Age), 1) AS avg_age
FROM normal
GROUP BY City
ORDER BY people DESC
```

- 単一のクエリ（`SELECT`、`WITH`、`VALUES` など）の結果は **SQL Result**
  シートに表示され、そのシートに切り替わります。別のクエリを実行すると置き換わります。
- それ以外の文は実行され、「Done」などのメッセージで結果を知らせます。複数の文が行を返す場合、シートとして表示されるのは単一のクエリだけです。
- エラーはメッセージボックスで表示されます。
- ファイルを再読み込み（Revert または Auto Revert）すると、SQL Result シートを最新に保つため、最後のクエリを実行し直します。

## ファイルは読み取り専用

ファイルは読み取り専用で開くので、SQL でデータベースやファイルを変更することはできません。SQL では問い合わせと一時テーブル・一時ビューの操作を許可しています。一時テーブルや一時ビューは作成・更新・削除でき、ウィンドウを閉じると消えます。

```sql
CREATE TEMP TABLE adults AS SELECT * FROM normal WHERE Age >= 20;
```

SQL による外部ファイルへの書き込み（`COPY ... TO`、`EXPORT`、`VACUUM INTO`）、別のデータベースの接続、拡張機能の読み込み、エンジン設定の変更は拒否します。クエリの結果を保存するには **Export View...**（ターミナル UI では `w`）を使ってください。

## 履歴

実行したクエリはすべて履歴に保存され、すべてのウィンドウと以降のセッションで共有されます。

- GUI：**History** ボタンで以前のクエリを一覧します。エディタ内では
  `Ctrl+Up` / `Ctrl+Down` で順にたどれます。
- ターミナル UI：SQL プロンプトで `Up` / `Down`。

履歴は `sql_history` という名前のファイルに保存されます。

| プラットフォーム | 場所                                                         |
| ---------------- | ------------------------------------------------------------ |
| Linux            | `$XDG_STATE_HOME/tableskim/`（通常は `~/.local/state/tableskim/`） |
| macOS            | `~/Library/Application Support/TableSkim/`                   |
| Windows          | `%APPDATA%\TableSkim\`                                        |

## 補完

`Tab` でカーソル直前の単語を補完します。対象はテーブル名・ビュー名、列名、
SQL のキーワードです。`名前.` の後ではそのテーブルの列を候補にし、`FROM` や
`JOIN` で付けた別名も理解します。たとえば `SELECT n. FROM normal AS n` の
`SELECT n.` では `normal` の列を補完します。

## ヒント

- SQL Result シートでもほかのシートと同様に並べ替えやフィルタができるので、
  `SELECT * FROM テーブル WHERE ...` のような簡単なクエリで足りることも多く、結果は Filter ペインで絞り込めます。
- 大きな CSV ファイルを SQL で扱いたいときは、先に Parquet か DuckDB に変換し（**File → Convert to Parquet / DuckDB...**、ターミナル UI では `P` /
  `D`）、変換したファイルを開いてください。
