# Example datasets

[← Contents](README.md)

The manual uses the public datasets in the repository's `demo/examples/` for its basic operations and charts. Download them from [Datasets, sources and licenses](https://github.com/informationsea/tableskim/tree/main/demo/examples). The first row holds column names. Open compressed files directly, without extracting them.

| File | Rows | Contents and uses |
| --- | --- | --- |
| `iris.csv` | 150 | Three iris species, 50 flowers each. Basic operations, correlation and grouping. Lengths and widths are in cm. |
| `penguins.csv` | 344 | Three penguin species. Box plots of body mass and missing values. Lengths are in mm; mass is in g. |
| `hour.csv.xz` | 17,379 | Hourly Bike Sharing records. Date histograms and density of temperature against rentals. |
| `flights.csv.xz` | 336,776 | Flights departing New York in 2013. Departure and arrival delays in a larger table. Delays are in minutes; negative values mean early departures or arrivals. |

## Try Iris

Run these commands from the repository root. In the installed GUI use **File → Open...**, or **Open...** in the web app, to select `iris.csv`.

```sh
tableskim demo/examples/iris.csv
tableskim -t demo/examples/iris.csv
tableskim --summary demo/examples/iris.csv
```

The columns are `sepal_length`, `sepal_width`, `petal_length`, `petal_width` (all in cm), and `species`.

1. **Search** for `setosa`: there are 50 hits.
2. Apply **Filter** conditions `species = virginica` and `petal_length ≥ 6` with **Match: All**: 11 of 150 rows remain. Use **Clear Filter** to reset.
3. Open a histogram of `petal_length` and choose **Stack by → species** to see each species' contribution.
4. In a scatter plot choose **X → petal_length**, **Y → petal_width**, **Color by → species**. The overall correlation is about 0.9629. It also reflects differences between species; filter to one species to study its own relationship.
5. In **Group By**, group by `species`, aggregate `petal_length`, and select **mean**, **min**, **max**. The means are 1.462 (setosa), 4.260 (versicolor), and 5.552 cm (virginica).
6. For SQL, use **Convert to Parquet / DuckDB...** (`P` in the TUI) and open `iris.parquet`. The query in [Running SQL](sql.md) gives the same means.

See [GUI](gui.md), [TUI](tui.md) or [Web](web.md) for the controls, and [Charts and Group By](charts.md) for how to read the results.

## Check missing values and units

`penguins.csv` has two missing body masses and two missing flipper lengths. A plot of `flipper_length_mm` against `body_mass_g` draws 342 penguins and skips two rows. Overall correlation includes size differences between species; distinguish it from correlation within a species.

In `hour.csv.xz`, `cnt` counts rentals in one hour and `temp` is normalized temperature, not degrees Celsius. `season` and `weathersit` are numeric category codes. Box plots need text on X; do not interpret those codes as continuous measurements. `dteday` is a date, with multiple hourly records for the same day.

`flights.csv.xz` represents missing values with the text `NA`. For delay analysis, filter with `dep_delay is a number` and `arr_delay is a number` to keep numeric pairs.

Examples specific to file formats, such as Excel date formats and multiple sheets, also use generated files in `demo/data/`. The public dataset records remain unchanged; screenshots show views and summaries. Iris and Bike Sharing are CC BY 4.0; Penguins and Flights are CC0 1.0. The link above includes sources and credits.

Iris: Fisher, R. (1936), [DOI: 10.24432/C56C76](https://doi.org/10.24432/C56C76). Bike Sharing: Fanaee-T, H. (2013), [DOI: 10.24432/C5W894](https://doi.org/10.24432/C5W894). Both are [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Penguins: Kristen B. Gorman / Palmer Station Antarctica LTER, palmerpenguins (Horst, Hill and Gorman, 2020). Flights: Hadley Wickham, nycflights13 (records from the US Bureau of Transportation Statistics). Both are [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
