# TableSkim 全体レビュー 引き継ぎ資料

- 対象コミット: `0d74bc4`（main、バージョン 0.3.5）
- 作業ブランチ: `ccr-5e6e37aa-y00aeo`（コミットなし。リポジトリへの変更はこのファイルだけで、未コミット）
- レビュー実施日: 2026-11-13 〜 15

## 1. 現在の状態

レビューは完了し、修正には一切着手していません。次の作業は「どの指摘を直すか決めて修正する」ことです。

重大度「高」の欠陥（データ競合、use-after-free、境界外アクセス、CI の権限漏れなど）は見つかっていません。指摘はすべて「中」以下です。

### 実行して確認したこと

| 項目 | 結果 |
| --- | --- |
| `cmake --preset debug` と `cmake --build --preset debug` | 成功。自前コードの警告なし（警告 12 件は RE2 が Abseil の非推奨 API を使うもの） |
| `ctest --preset debug -j 4` | 380 件すべて成功（`tableskim-tui-tests`、`tableskim-gui-tests` を含む） |
| ODS の指摘 A-1、A-2 | 細工ファイルで再現（手順は 4 章） |

### 実行していないこと

- `asan` プリセット、`wasm-*` プリセット、Web アプリと VS Code 拡張のビルド・テスト
- ODS 以外の指摘の再現。下表の「確認」列が「コード」のものは該当コードを読んで確かめただけ、「報告」のものは領域別レビューの報告のままで、引き継ぎ者による裏取りが必要です

### 環境についての注意

- ビルド済みの `build/debug/` はこのセッションのコンテナにあり、コンテナが回収されると消えます。初回の構成とビルドは 4 コアで 1 時間前後かかりました（DuckDB のビルドが大半）。
- 領域別レビューを 8 並列で走らせたところ、利用上限に 3 回達しました。並列でサブエージェントを使う場合は 2〜3 件ずつにするのが安全です。
- GitHub の MCP サーバーはこのセッションでは接続に失敗していました（プロキシが 403）。PR の作成などが必要なら、新しいセッションで接続を確かめてください。

## 2. 着手順の提案

小さく確実に直せて効果が大きい順です。

1. A-1、A-2（ODS のメモリ枯渇とオーバーフロー）: 数行の検査追加。再現ファイルあり。
2. B-1（GUI の Quit で編集が消える）: `confirm_unsaved()` を「尋ねる」と「実行する」に分ける。
3. C-1（pandas 用コードの正規表現）: 完全一致パターンを `==` で出力する。
4. D-4（VS Code のファイル名の `$`）: `replace` の第 2 引数を関数にする 1 行修正。
5. E-1〜E-4（リリースワークフローの固定とガード）: YAML の変更のみ。
6. F-1〜F-3（ドキュメントの ODS・キー・`K` の反映漏れ）: 英日両方を直す。`tests/markdown_tests.cxx` がリンクを検査するので、修正後にテストを実行する。

`src/common/` を変更したら、AGENTS.md のとおりテストを追加し、テスト一式を実行してください。メモリ処理に触れる A 群は `asan` プリセットでも確認するのが望ましいです。

## 3. 指摘一覧

### A. 細工したファイルで落ちる・固まる

| ID | 重大度 | 場所 | 内容 | 修正案 | 確認 |
| --- | --- | --- | --- | --- | --- |
| A-1 | 中 | `src/common/table_ods.cxx:382-391, 573-597`（検査は `:666`） | セルを読んでいる間 `text_` が無制限に伸びる。`kMaxRowBytes` の検査は `endCell()` まで行われない。12 KB の .ods で 4 GB 超を要求する | `addText()` / `addExplicit()` で `kMaxRowBytes - row_bytes_` と比べ、既存の "row is too large" エラーを投げる。索引パスは文字列を溜めない | 再現 |
| A-2 | 低〜中 | `src/common/table_ods.cxx:139, 180` | 時間値の `whole`（最大 1e12）に `unit_millis`（最大 86,400,000）を掛けてから上限を検査するので、符号付きオーバーフロー（未定義動作） | 掛ける前に `whole > (kMaxMillis - millis) / unit_millis` で弾く | 再現 |
| A-3 | 低〜中 | `src/common/table_ods.cxx:22-24, 499-514, 735-740` | 行数と結合セルの上限がシート単位で、シート数に上限がない。小さなファイルが大量の結合記録や 1,670 万行の空シートを要求できる | 結合セルの上限を文書単位にし、結合の行範囲を値のある最終行付近に切り詰める | 報告（下の注を参照） |
| A-4 | 中 | `src/common/table_excel.cxx:502, 406, 549` | OpenXLSX が ZIP の各エントリを丸ごと展開して DOM 化する。展開サイズの上限がない。WebAssembly では特に早く限界に達する | `document.open()` の前に既存の `ZipArchive` で中央ディレクトリを読み、非圧縮サイズが上限を超えるものを拒否する | 報告 |

注（A-3）: `rows.ods`（非空 16,384 列の行を 40 億回繰り返す 509 バイトのファイル）は、4 GB のメモリ制限下で `--summary` が `bad_alloc` になりました。ただしこれは A-3 が述べる結合セルの経路ではなく、繰り返し行の経路です。`--summary` は先頭行以外を保持しないはずなので、どこで確保が膨らむのかは未調査です。A-3 とは別の問題の可能性があります。

### B. 編集内容の誤書き込み・消失

| ID | 重大度 | 場所 | 内容 | 修正案 | 確認 |
| --- | --- | --- | --- | --- | --- |
| B-1 | 中 | `src/gui/gui_main.cxx:127-146`、`src/gui/gui_impl.cxx:1869-1893` | Quit は各ウィンドウに順に尋ね、「Don't Save」でその場で `discardEdits()` する。後のウィンドウで Cancel すると終了は中止されるが、先のウィンドウの編集は消えている | 全ウィンドウの回答を集めてから保存・破棄する。または終了経路では破棄しない（ウィンドウを閉じればブックは捨てられる） | コード |
| B-2 | 中 | `src/tui/tui_app.cxx:3800-3846` | 編集プロンプトはビュー行 `edit_row_` を覚える。ソートやフィルタの適用待ち中に編集を始めると、確定時に別のソース行へ書き込む | プロンプトを開いたときのビュー（またはソース行）を覚え、確定時に違っていたら取り消す。あるいは `requestedView() != view()` の間は編集を拒否する | 報告 |
| B-3 | 低〜中 | `src/gui/gui_table_view.cxx:1531-1538, 1342-1371` | 固定列のセル編集（`fl_input`）とセルメニューが、入れ子ループの前に取った座標・値を、ビューが変わっていないか確かめずに使う | `fl_input` の後でモデルとビューを比べ、違えば書き込まない | 報告 |
| B-4 | 低〜中 | `src/gui/gui_impl.cxx:1945-1963` | 保存後の `open()` の結果を見ていない。失敗すると `table_book` が null のまま古い表示が残る | 結果を確かめ、失敗したら元のパスを開き直して警告する | 報告 |
| B-5 | 低 | `src/common/compress.cxx:44-49`、`src/common/file_util.cxx:113-131` | 保存時に `fsync` せずリネームする。直後の電源断で切り詰められたファイルが残りうる | リネーム前に `fsync` / `FlushFileBuffers` | 報告 |

### C. 誤った結果

| ID | 重大度 | 場所 | 内容 | 修正案 | 確認 |
| --- | --- | --- | --- | --- | --- |
| C-1 | 中 | `src/common/cell_format.cxx:728-731` → `src/common/analysis_code.cxx:1478-1481` | グループのフィルタは RE2 のパターン `(?-i)\A<引用>\z`。pandas 用コードはこれをそのまま `str.contains` に渡すので `re.error` になる（Python 3.13 で確認済みとの報告）。polars と R は受け付ける | コード生成で完全一致の形を見分けて `==` / `!=` を出力する。グループ条件を含む解析コードのテストを足す | コード |
| C-2 | 中 | `src/common/group_by.cxx:49-64`、`src/common/table.cxx:583-596` | 日付セル、日付形式の文字列、数値が混在するキー列で `KeyLess` が厳密弱順序にならない（文字列 < 日付 < 99999 < 文字列）。`std::map` の動作が未定義。メモリ内ソートと `column_stats.cxx:29-42` の `partial_sort` も同じ比較を使う | まず種類（時刻、数値、文字列）で順位を付け、同じ種類の中だけで比べる | 報告（未再現） |
| C-3 | 中 | `src/common/table.cxx:2075, 2113-2127` | プッシュダウン型ソースでビュー適用中に起きた例外は、原因を問わずビューのエラーとして扱われ、ソート・フィルタが外れる。エクスポート中の読み取り失敗でも画面のビューが戻る | `failed_view` を `applyView()` とその直後の読み取りに限り、それ以降は読み取りエラーまたは走査の失敗として扱う | コード |
| C-4 | 低〜中 | `src/common/cell_format.cxx:611-616, 667-680` | `parseTemporal()` が読めない年（0000 未満、9999 超）の境界を黙って落とす。そのビンのフィルタが全行を通し、「ビンのフィルタは数えた行だけを残す」という不変条件が崩れる | `parseTemporal()` を符号付き・5 桁以上の年に広げるか、範囲外を「その他」に数える | 報告 |
| C-5 | 低 | `src/common/histogram.cxx:45, 83-90, 104-109, 449-451`、`src/common/table.cxx:683` | 時刻を `double` に直してビンに入れるので ±2^53 µs の外でフィルタと食い違う。日数×マイクロ秒が int64 を超える。`std::chrono::year` は ±32767 まで | 時刻の端を `int64_t` で持つ。対応範囲外の時刻は「その他」に数える | 報告 |
| C-6 | 低 | `src/common/cell_format.cxx:864-868, 806-807` | `"%.*f"` を 64 バイトのバッファに書くので、1e63 以上の値が切れてフィルタの境界が別の数になる | 2 段階の `snprintf` でバッファを決める | 報告 |
| C-7 | 低 | `src/common/table.cxx:1278`、`src/common/table.hxx:374-378` | 同じ指定の `setView()` は何もしない。編集後に同じ列で並べ直せず、ヘッダーの説明と食い違う | 編集後に「ビューが古い」フラグを立てて早期リターンを避ける。またはコメントを直す | コード |
| C-8 | 低〜中 | `src/common/table.cxx:2253-2274` | ランダムアクセス型ソースの `columnTypes()` が「先頭の行」ではなく「最初に読んだブロック」を数え、ビュー変更や追い出しの後に同じ行を二重に数える | ビューが空のときの先頭ブロックだけを数える。またはビュー変更で数え直す | 報告 |
| C-9 | 低 | `src/common/table_duckdb.cxx:620-625` | UBIGINT / HUGEINT の整数フィルタは SQL では厳密比較、メモリ内では `double` 比較で、結果が食い違う | どちらを規則にするか決め、`checkFilterMatches()` で検査する | 報告 |
| C-10 | 低 | `src/common/table_summary.cxx:170-181, 405-423` | `--summary` の Markdown 出力が `|` しかエスケープしない。`a\|b` を含むセルで列がずれる | `\` を先にエスケープし、名前とエラー文にも同じ処理を通す | 報告 |

### D. Web アプリ / WebAssembly セッション / VS Code 拡張

| ID | 重大度 | 場所 | 内容 | 修正案 | 確認 |
| --- | --- | --- | --- | --- | --- |
| D-1 | 中 | `src/wasm/web_session.cxx:1170-1184`、`src/common/cell_format.cxx:356-414` | `copy` の範囲がセッション側で無制限（`rows` には上限がある）。ページ側は行数だけ制限している（`web/src/App.tsx:508-516`） | `right` を列数、`bottom` を表示行数に切り詰め、セル数かバイト数に上限を設ける | コード |
| D-2 | 中 | `src/wasm/web_session.cxx:891-911` | `open` が引数の検証と `TableBook::open()` の前に `close()` する。失敗すると表示中のファイルも閉じられる | 新しいブックをローカル変数に開いてから差し替える | 報告 |
| D-3 | 中 | `src/wasm/web_session.cxx:1258-1275, 1277-1287` | `sql` がセッションスレッドを問い合わせ完了まで止める。その間は `rows` も `cancelJob` も処理されない | ジョブ形式にする（すぐ応答し、完了はイベントで知らせる） | 報告 |
| D-4 | 中 | `vscode/src/editor.ts:316` | `page.replace('<head>', ...)` の置換文字列にファイル名が入るので、`$&` や `$'` が展開される。CSP があるのでスクリプト実行には至らない | `page.replace('<head>', () => ...)` と関数形式にする | コード |
| D-5 | 中 | `web/src/App.tsx:540-557`、`web/src/grid/controller.ts:165-170` | シート切り替え直後、前のシートへの `rows` 応答が新しいシートの行として保存されうる | `fetchRows` でシート ID も照合し、`setInfo` の新シート時に `generation` を進める | 報告（未再現） |
| D-6 | 中 | `web/src/components/fileJob.ts:62-64`、`web/src/vscode/port.ts:55-61`、`vscode/src/editor.ts:181-186, 330` | VS Code ではエクスポートしたファイルが丸ごと webview を往復する。数百 MB で失敗しうる | 拡張ホスト側でエンジンの応答を直接書き出す | 報告 |
| D-7 | 中 | `web/src/App.tsx:516-517`、`web/src/components/CodeDialog.tsx:83` | クリップボードへの書き込みが `await` の後で、ユーザー操作の外になる。Safari などで拒否されうる | `ClipboardItem` に Promise を渡して操作内で呼ぶ | 報告 |
| D-8 | 低〜中 | `web/src/client.ts:47-62`、`vscode/src/editor.ts:178` | エンジン停止後の新しい要求が永久に返らない | 失敗状態を持ち、以後の `call()` をすぐ拒否する | 報告 |
| D-9 | 低〜中 | `web/src/grid/controller.ts:17, 336-350, 357` | 約 3 億行以上でスクロール 1 ピクセルが表示範囲を超え、届かない行ができる | 先頭行をコントローラで厳密に持つ | 報告 |
| D-10 | 低 | `web/src/grid/controller.ts:477-482` | 行の取得に失敗すると 100 ms ごとに際限なく再試行する | 間隔を延ばし、意図的な "stale" では再試行しない | 報告 |
| D-11 | 低 | `src/wasm/web_session.cxx:1580-1590, 1681-1707` | `startExport` / `startGroupBy` が検証の前に `stopJob()` する。不正な要求で前のジョブの結果が消える | 検証を先にする | 報告 |
| D-12 | 低 | `src/wasm/README.md:208-244`、`web/src/protocol.ts` | プロトコル表の抜け: `startScatter` の `points`、`setView` の `addNarrowing`、`defaultScatter` の `firstRow` / `lastRow`、`startGroupBy` の `hidden`（`protocol.ts` にもない） | 表と型に足す | 報告 |
| D-13 | 低 | `vscode/src/editor.ts:63-75, 243-261`、`vscode/src/extension.ts:22-25`、`vscode/package.json:147-153` | inode が 0 のファイルシステムで保存先を誤って拒否。再読み込みの確認がエディタごとに出る。`tableskim.open` が何もしない場合がある | 各報告の修正案どおり | 報告 |

### E. ビルド・CI・リリース

| ID | 重大度 | 場所 | 内容 | 修正案 | 確認 |
| --- | --- | --- | --- | --- | --- |
| E-1 | 中 | `.github/workflows/release.yml:482, 293`、`.github/workflows/ci.yml:55` | `softprops/action-gh-release@v2` と `ilammy/msvc-dev-cmd@v1` がタグ指定。前者は `contents: write` を持つジョブで動く | コミット SHA で固定し、バージョンをコメントに書く | コード |
| E-2 | 中 | `cmake/Dependencies.cmake:44, 83, 212, 226, 242, 273, 288, 427, 447, 458` | Git 依存 10 件がタグ名だけで固定されている | `GIT_TAG` をコミット SHA にする | 報告 |
| E-3 | 中 | `.github/workflows/release.yml:529-535` | VS Code の CHANGELOG がバージョンを含むかの検査が、GitHub リリース公開後に走る | 検査を `check-version` に移す | 報告 |
| E-4 | 低 | `.github/workflows/release.yml:69-72, 113-116, 183-186, 235-240` | Linux と macOS のアップロードに `if-no-files-found: error` がない | 4 か所に足す | 報告 |
| E-5 | 低 | `.github/workflows/ci.yml:125, 279, 342, 378` | `workflow_dispatch` なら main 以外でもキャッシュを保存する。AGENTS.md の記述と食い違う | 保存を `github.ref == 'refs/heads/main'` に限る | 報告 |
| E-6 | 低 | `.github/workflows/ci.yml:20-22, 428, 518` | main の連続マージで中間の実行が取り消される（コメントと食い違う）。`curl ... | jq` が失敗してもステップが通る | グループに SHA を入れる。変数に受けて空を弾く | 報告 |
| E-7 | 中〜低 | `cmake/Licenses.cmake:23-27, 103`、`cmake/WebZip.cmake:31-35` | 名前が分からない依存を黙って飛ばす。csv-parser の同梱ヘッダーや Emscripten のシステムライブラリの表記がない可能性 | 理由のない欠落は `FATAL_ERROR` にする。項目を足す | 報告 |
| E-8 | 低 | `vscode/package.json:5`、`web/package.json:4` | バージョンが 0.3.3 と 0.3.0（本体は 0.3.5）。パッケージ時には上書きされる | バージョン更新時に一緒に上げるか、`check-version` で比べる | コード |
| E-9 | 低 | `CMakeLists.txt:464, 595-602`、`packaging/` | `CFBundleVersion` が常に `"1"`。マニュアルの出力ディレクトリを掃除しない。AppStream の metainfo がない | 各報告の修正案どおり | 報告 |
| E-10 | 低 | `packaging/windows/make-msix.ps1:36-43` | MSIX のバージョンが `0.3.5.0`。メジャー 0 はストアで拒否される可能性（未確認） | ストアの規則を確かめる | 報告（未確認） |

### F. ドキュメント

| ID | 重大度 | 場所 | 内容 |
| --- | --- | --- | --- |
| F-1 | 中 | `README.md:96, 166`、`docs/en/tui.md:378`、`docs/en/keyboard-shortcuts.md:104`、`docs/en/gui.md:521`、`docs/en/getting-started.md:143, 170`、`docs/en/web.md:105`、`AGENTS.md:197-199`、`docs/ja/` の同じ箇所 | 先頭行オプションの対象に ODS がない。コードでは対象（`src/common/table_ods.cxx:936`、`src/tui/tui_app.cxx:85`）。`docs/en/editing.md:9-13` の編集不可一覧と `docs/en/filtering.md:32` にも ODS がない |
| F-2 | 中 | `docs/en/keyboard-shortcuts.md:140-162`、`docs/ja/keyboard-shortcuts.md` | グラフ用のキー `g`/`G`、`Tab`/`Shift+Tab`、`c`/`C`、`K` がない（実装は `src/tui/tui_app.cxx:2330-2681`） |
| F-3 | 中 | `docs/ja/charts.md`（153-156 行付近） | 英語版 `docs/en/charts.md:252-253` にある `K` の項目がない |
| F-4 | 中 | `vscode/README.md:3-4, 37` | 既定で開く形式に ODS がない（`vscode/package.json:65` にはある） |
| F-5 | 中 | `docs/`、`site/` | VS Code 拡張と `.vsix` がマニュアルにもサイトにも出てこない |
| F-6 | 低 | `docs/en/web.md:139-147, 252`、`docs/en/charts.md:261-262`、`docs/en/exporting.md:63-64`、`README.md:181-182, 322-327`（日本語版も） | サイドバーの Search タブ抜け、集計の std dev 抜け、「画像保存は GUI のみ」（Web でも可能）、変換後の挙動の矛盾、依存一覧の expat 抜け、パッケージ一覧の Web ZIP 抜け |
| F-7 | 低 | `AGENTS.md:8`、`src/wasm/README.md:262`、`site/index.html:203, 220-239` | 「two frontends」の記述、`demo/examples/` の説明なし、古いテスト件数、サイトのページ一覧の抜け |
| F-8 | 低 | `docs/ja/` | 「最頻値」（複数の値を指す箇所）と、「テキスト」「文字列」の揺れ |

### G. その他（低優先度）

- TUI: ダイアログやグラフを開いている間に Group By や SQL の結果が届くと、別シートにフィルタ・書式・`F` が効く（`src/tui/tui_app.cxx:3630-3653, 2375-2376`）。グラフ表示中のマウス操作が下の表に届く（`:1818-1821`）。固定列が画面幅を超えると操作不能（`src/tui/tui_table_view.cxx:286-316`）。括弧付きペーストが異常終了時に戻らない（`src/tui/tui_main.cxx:66-76`）。
- Follow: CP932 のパイプ区切り、CR のみの改行、64 MiB 超の 1 レコードで追記行を拾えない（`src/common/table_csv.cxx:222-261, 967-973`）。
- GUI: `HelpWindow::switchLanguage` が `utf8Path` を使っていない（`src/gui/gui_help.cxx:314`、コードで確認済み）。`Fl::awake` の戻り値を見ていない。最近のファイルを `sync_menu_items()` のたびに読み直す。未使用のスタブが残っている。
- コア: ワーカースレッドに catch-all がない（`src/common/table.cxx:2044, 949-954`）。ブック破棄時に待機中の `done` が呼ばれない（`:933, 951`）。`requestRange()` に上限がない（`:2186-2205`）。
- SQLite: `trusted_schema` が既定のまま、`tableskim_match` が演算子の値を検査しない（`src/common/table_sqlite.cxx:56, 190-207`）。

## 4. 再現手順（A-1、A-2）

次のスクリプトを `ods.py` として保存し、空のディレクトリを出力先にして実行します。

```python
import sys, zipfile
NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
      'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
      'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"')
def ods(path, body):
    xml = ('<?xml version="1.0"?><office:document-content %s><office:body><office:spreadsheet>%s'
           '</office:spreadsheet></office:body></office:document-content>' % (NS, body))
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(zipfile.ZipInfo('mimetype'), 'application/vnd.oasis.opendocument.spreadsheet')
        z.writestr('content.xml', xml, zipfile.ZIP_DEFLATED)
out = sys.argv[1]
n = int(sys.argv[2])
ods(out + '/bomb.ods', '<table:table table:name="s"><table:table-row><table:table-cell office:value-type="string"><text:p>a' +
    '<text:s text:c="65536"/>' * n + '</text:p></table:table-cell></table:table-row></table:table>')
ods(out + '/time.ods', '<table:table table:name="s"><table:table-row>'
    '<table:table-cell office:value-type="time" office:time-value="P999999999999D"><text:p>x</text:p></table:table-cell>'
    '<table:table-cell office:value-type="time" office:time-value="PT999999999999H999999999999M"><text:p>y</text:p></table:table-cell>'
    '</table:table-row></table:table>')
ods(out + '/rows.ods', '<table:table table:name="s"><table:table-row table:number-rows-repeated="4000000000">'
    '<table:table-cell table:number-columns-repeated="16384" office:value-type="string"><text:p>abcdefghijklmnopqrstuvwxyz0123456789</text:p></table:table-cell>'
    '</table:table-row></table:table>')
```

```sh
mkdir -p /tmp/ods-repro && python3 ods.py /tmp/ods-repro 200000
( ulimit -v 4000000; ./build/debug/tableskim --summary /tmp/ods-repro/bomb.ods )
./build/debug/tableskim --summary /tmp/ods-repro/time.ods
( ulimit -v 4000000; ./build/debug/tableskim --summary /tmp/ods-repro/rows.ods )
```

観測した結果:

- `bomb.ods`（12,172 バイト）: `Failed to open ...: std::bad_alloc`。メモリ制限を外すと約 13 GB を要求する計算なので、必ず `ulimit -v` を付けてください。
- `time.ods`: 1 列目の見出しが `-1620477880176:-9:-18.-80` になる（オーバーフローした値）。
- `rows.ods`（509 バイト）: `Failed to read ..., sheet s: std::bad_alloc`。原因は未調査です（A-3 の注を参照）。

## 5. レビューで読めていない範囲

- GUI: `gui_theme.cxx`、`gui_fonts.cxx`、ヒストグラムと散布図の描画・マウス処理、フィルタと統計のペインの前半
- 共通: `markdown.{hxx,cxx}`、`help_docs.{hxx,cxx}`、`search_hits.*`、`src/tools/`、`sql_text.cxx` の補完と字句解析の境界条件
- バックエンド: OpenXLSX・miniz・csv-parser の内部、`commitSave()`、Windows 専用の経路、DuckDB の SQL 変換とメモリ内比較の網羅的な照合
- Web: フィルタ・書式・エクスポート・変換・Group By の各ダイアログ、グラフ本体、応答の項目単位での `protocol.ts` との照合、アクセシビリティ
- WASM: `wasm_probe.cxx`
- ドキュメント: `site/ja/download.html`、`site/style.css`、`vscode/DEVELOPMENT.md`、`docs/ja` の大半（構造の比較のみで、文章は精読していない）
- ビルド: `USE_SYSTEM_*` と `X11_ONLY` の組み合わせ（読んだだけ）、`tools/generate-cp932-table.py`、`demo/create-demo-data.py` の本体

## 6. 問題がなかったと確認した点

修正時に壊さないための参考です。

- スケジューラの起床、ロック順序（ブック → モデル → スケジューラ）、世代カウンタによる古い結果の破棄、`Subscription` の寿命
- ZIP リーダーの境界検査、伸長ストリームの切り詰め検出、文字コード変換のチャンク境界
- SQL の識別子と文字列のクォート（`ATTACH`、`read_parquet`、`read_json` のパスを含む）
- GUI のダイアログはすべてリテラルの書式文字列を使う。ウィンドウは `tableWindowOf()` / `frontTableWindow()` で探している
- Web: `dangerouslySetInnerHTML`、`innerHTML`、`eval` はない。webview の CSP は `default-src 'none'`
- CI: `pull_request_target` なし、`run:` への `github.event.*` の埋め込みなし、書き込み権限はジョブ単位
- マニュアル: コマンドラインオプション、TUI のキー、GUI のメニューとショートカット、各種の上限値はコードと一致。相対リンクと画像はすべて解決する
