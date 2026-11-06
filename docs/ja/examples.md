# サンプルデータ

[← 目次](README.md)

このマニュアルの基本操作とグラフの例は、リポジトリの `demo/examples/` にある公開データを使います。[データと出典・ライセンス](https://github.com/informationsea/tableskim/tree/main/demo/examples) から入手できます。先頭行は列名として扱います。圧縮ファイルは展開せず、そのまま開けます。

| ファイル | 行数 | 内容と使いどころ |
| --- | --- | --- |
| `iris.csv` | 150 | 3 品種のアヤメ、各 50 個体。基本操作、相関、品種ごとの集計。長さと幅は cm。 |
| `penguins.csv` | 344 | 3 種のペンギン。種ごとの体重の箱ひげ図、欠損値の確認。長さは mm、体重は g。 |
| `hour.csv.xz` | 17,379 | Bike Sharing の毎時記録。日付のヒストグラム、気温とレンタル数の密度分布。 |
| `flights.csv.xz` | 336,776 | 2013 年のニューヨーク発の便。大きい表での出発遅延と到着遅延の比較。遅延は分、負の値は定刻より早いことを表す。 |

## Iris で試す

リポジトリのルートで次のように起動します。インストール済みアプリでは **File → Open...**、Web アプリでは **Open...** で `iris.csv` を選べます。

```sh
tableskim demo/examples/iris.csv
tableskim -t demo/examples/iris.csv
tableskim --summary demo/examples/iris.csv
```

列は `sepal_length`（がく片の長さ）、`sepal_width`（がく片の幅）、`petal_length`（花びらの長さ）、`petal_width`（花びらの幅）、`species`（品種）です。

1. **Search** で `setosa` を検索すると 50 件見つかります。
2. **Filter** で `species = virginica` と `petal_length ≥ 6` を **Match: All** で適用すると、150 行中 11 行が残ります。**Clear Filter** で元に戻します。
3. `petal_length` のヒストグラムを開き、**Stack by → species** にすると品種別の分布が見えます。
4. 散布図で **X → petal_length**、**Y → petal_width**、**Color by → species** を選びます。全体の相関係数は約 0.9629 です。品種間の違いも含む相関なので、品種内を調べるときは品種で絞り込みます。
5. **Group By** で `species` をグループに、`petal_length` を集計対象にし、**mean**、**min**、**max** を選びます。平均は setosa が 1.462、versicolor が 4.260、virginica が 5.552 cm です。
6. SQL を試すには **Convert to Parquet / DuckDB...**（TUI は `P`）で変換し、`iris.parquet` を開きます。[SQL の実行](sql.md) のクエリで同じ平均を確認できます。

操作の詳しい説明は [GUI](gui.md)、[TUI](tui.md)、[Web](web.md)、結果の読み方は [グラフと集計](charts.md) を参照してください。

## 欠損値と単位を確認する

`penguins.csv` の体重とひれの長さには、それぞれ 2 個体分の欠損値があります。`flipper_length_mm` と `body_mass_g` の散布図では 342 個体を描き、2 行を除外します。種ごとに体格が違うので、全体の相関と種内の相関を区別します。

`hour.csv.xz` の `cnt` は 1 時間のレンタル数、`temp` は正規化された気温で、摂氏ではありません。`season` と `weathersit` は数値のカテゴリコードです。箱ひげ図の X にはテキストの列を使うので、これらのコードをそのまま連続量として解釈しないでください。`dteday` は日付ですが、同じ日に複数の毎時記録があります。

`flights.csv.xz` は欠損を文字列 `NA` で表しています。遅延の分析では `dep_delay is a number` と `arr_delay is a number` で数値の行に絞り込めます。

日付・Excel の表示形式や複数シートなど、ファイル形式特有の説明では `demo/data/` の生成データも使います。公開データのレコードは変更しません。画像は表示や集計の結果です。Iris と Bike Sharing は CC BY 4.0、Penguins と Flights は CC0 1.0 で、出典とクレジットは上記のリンク先にあります。

Iris: Fisher, R. (1936), [DOI: 10.24432/C56C76](https://doi.org/10.24432/C56C76)。Bike Sharing: Fanaee-T, H. (2013), [DOI: 10.24432/C5W894](https://doi.org/10.24432/C5W894)。どちらも [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) です。Penguins: Kristen B. Gorman / Palmer Station Antarctica LTER、palmerpenguins（Horst、Hill、Gorman、2020）。Flights: Hadley Wickham、nycflights13（元データ: US Bureau of Transportation Statistics）。この 2 件は [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) です。
