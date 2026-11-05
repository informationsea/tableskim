# Public datasets for analysis examples

These CSV files are copies of public datasets, with their original headers,
values and missing entries preserved. Row counts exclude the header.

Files over 1 MB (1,000,000 bytes) are stored compressed. For each file,
gzip (`-9 -n`), bzip2 (`-9`), xz (`-9e`) and zstd (`--ultra -22`) were
compared, and the smallest output was kept. Every candidate was decompressed
and checked against the original bytes. TableSkim opens these files directly.

| File | Original bytes | Compressed bytes | Size reduction |
| --- | ---: | ---: | ---: |
| `flights.csv.xz` | 33,206,996 | 4,809,408 | 85.52% |
| `hour.csv.xz` | 1,156,736 | 171,840 | 85.14% |

| File | Rows | Correlation / linear regression | Box plot (group → value) |
| --- | ---: | --- | --- |
| `penguins.csv` | 344 | `flipper_length_mm` → `body_mass_g` | `species` → `body_mass_g` |
| `iris.csv` | 150 | `petal_length` → `petal_width` | `species` → `petal_length` |
| `flights.csv.xz` | 336,776 | `dep_delay` → `arr_delay` | `carrier` → `arr_delay` |
| `hour.csv.xz` | 17,379 | `temp` → `cnt` | `season` or `weathersit` → `cnt` |

Penguins has missing values; omit rows missing the variables used in an
analysis. Its overall correlation also reflects differences between species.

Flights has 19 data columns plus the Rdatasets `rownames` column. Delays are
in minutes, with negative values indicating early departures or arrivals.
Missing values are represented by `NA`; omit rows missing the variables used
in an analysis.

Bike Sharing's `hour.csv.xz` contains hourly rental counts (`cnt`) and normalized
weather measurements. Temperature (`temp`) is normalized, rather than in
degrees Celsius. `season` and `weathersit` are numeric category codes; treat
them as categories for grouped box plots. Demand also varies by hour and
working day, so a simple temperature regression has limitations.

## Sources

- Penguins: [seaborn-data CSV](https://raw.githubusercontent.com/mwaskom/seaborn-data/master/penguins.csv),
  from [Palmer Penguins](https://allisonhorst.github.io/palmerpenguins/).
- Iris: [seaborn-data CSV](https://raw.githubusercontent.com/mwaskom/seaborn-data/master/iris.csv).
- NYC Flights 2013: [Rdatasets CSV](https://raw.githubusercontent.com/vincentarelbundock/Rdatasets/master/csv/nycflights13/flights.csv),
  from R's `nycflights13` package.
- Bike Sharing: [UCI dataset documentation](https://archive.ics.uci.edu/dataset/275/bike+sharing+dataset),
  downloaded from the [Udacity mirror of hour.csv](https://raw.githubusercontent.com/udacity/deep-learning/master/first-neural-network/Bike-Sharing-Dataset/hour.csv).

## Data licenses and attribution

Only CC0 and CC BY 4.0 datasets are bundled in this directory. These data
retain their own terms, independently of TableSkim's software license.
Keep these attributions and license links with redistributed copies.

| File | License | Attribution and license evidence |
| --- | --- | --- |
| `penguins.csv` | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | Kristen B. Gorman and Palmer Station Antarctica LTER; palmerpenguins by Allison Marie Horst, Alison Presmanes Hill and Kristen B. Gorman (2020). [Upstream license and citation](https://allisonhorst.github.io/palmerpenguins/#license). |
| `flights.csv.xz` | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | Hadley Wickham, nycflights13; flight records from the US Bureau of Transportation Statistics. [Upstream license](https://nycflights13.tidyverse.org/#license). |
| `iris.csv` | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Fisher, R. (1936). Iris [Dataset]. UCI Machine Learning Repository. [DOI: 10.24432/C56C76](https://doi.org/10.24432/C56C76). [Upstream license](https://archive.ics.uci.edu/dataset/53/iris). |
| `hour.csv.xz` | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Fanaee-T, H. (2013). Bike Sharing [Dataset]. UCI Machine Learning Repository. [DOI: 10.24432/C5W894](https://doi.org/10.24432/C5W894). [Upstream license](https://archive.ics.uci.edu/dataset/275/bike+sharing+dataset). |

CC BY 4.0 permits redistribution and adaptation, including commercial use,
with appropriate credit, a license link and an indication of changes. Do not
impose additional restrictions on the licensed data. CC0 attribution is
provided for provenance.

The bundled files preserve the bytes from the download sources listed above
(after decompression where applicable); TableSkim has not edited their records.
Those mirrors may format the original data differently: Iris uses CSV column
headers and shortened species labels, Penguins uses a simplified set of columns,
and Rdatasets adds a `rownames` column to Flights. Flights and Bike Sharing
are additionally compressed with xz for storage.
