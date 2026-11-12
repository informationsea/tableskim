#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "analysis_code.hxx"
#include "file_util.hxx"
#include "table.hxx"
#include "test_helpers.hxx"

namespace
{
    const std::filesystem::path kDemoData = TABLESKIM_DEMO_DATA_DIR;

    struct Opened
    {
        std::shared_ptr<TableBook> book;
        std::shared_ptr<TableModel> model;
    };

    // The sheet once its first rows are read, so that the column types are known.
    Opened openSheet(const std::filesystem::path &path, const std::string &sheet = {})
    {
        Opened opened;
        opened.book = TableBook::open(path);
        opened.model = opened.book->sheet(sheet.empty() ? opened.book->sheetNames().front() : sheet);
        REQUIRE(opened.model);
        cellText(*opened.model, 0, 0);
        REQUIRE(waitFor([&]
                        {
            const auto types = opened.model->columnTypes();
            return !types.empty() && types[0].kind != ColumnKind::Unknown; }));
        return opened;
    }

    CodeSource normalSource()
    {
        CodeSource source;
        source.path = "/data/normal.csv";
        source.delimiter = ',';
        source.columns = {"ID", "Name", "Age", "City", "Salary", "Birthday", "Active", "Last Login"};
        source.kinds = {ColumnKind::Number, ColumnKind::Text,    ColumnKind::Number,  ColumnKind::Text,
                        ColumnKind::Number, ColumnKind::Date, ColumnKind::Boolean, ColumnKind::DateTime};
        return source;
    }

    bool contains(const std::string &text, const std::string &part)
    {
        if (text.find(part) != std::string::npos)
            return true;
        UNSCOPED_INFO("missing: " << part << "\nin:\n" << text);
        return false;
    }

    const size_t ID = 0, NAME = 1, AGE = 2, CITY = 3, SALARY = 4, BIRTHDAY = 5, ACTIVE = 6, LAST_LOGIN = 7;
} // namespace

TEST_CASE("codeSourceOf: format, options, names and kinds of an opened file")
{
    SECTION("CSV")
    {
        const Opened opened = openSheet(kDemoData / "normal.csv");
        const CodeSource source = codeSourceOf(kDemoData / "normal.csv", *opened.book, *opened.model);
        CHECK(source.format == CodeFormat::Text);
        CHECK(source.delimiter == ',');
        CHECK(source.header);
        CHECK(source.table.empty());
        CHECK(source.encoding == TextEncoding::Utf8);
        REQUIRE(source.columns.size() == 9);
        CHECK(source.columns[3] == "Age");
        CHECK(source.kind(0) == ColumnKind::Number);
        CHECK(source.kind(1) == ColumnKind::Text);
        CHECK(source.kind(7) == ColumnKind::Date);
        CHECK(source.kind(8) == ColumnKind::Boolean);
        CHECK(source.kind(100) == ColumnKind::Unknown);
        CHECK(source.columnName(100) == columnLetter(100));
    }
    SECTION("Compressed CP932 text")
    {
        const Opened opened = openSheet(kDemoData / "normal-cp932.csv.gz");
        const CodeSource source = codeSourceOf(kDemoData / "normal-cp932.csv.gz", *opened.book, *opened.model);
        CHECK(source.format == CodeFormat::Text);
        CHECK(source.delimiter == ',');
        CHECK(source.encoding == TextEncoding::Cp932);
    }
    SECTION("Excel names the sheet")
    {
        const Opened opened = openSheet(kDemoData / "multisheet.xlsx");
        const CodeSource source = codeSourceOf(kDemoData / "multisheet.xlsx", *opened.book, *opened.model);
        CHECK(source.format == CodeFormat::Excel);
        CHECK(source.table == opened.model->name());
    }
    SECTION("A delimiter given when opening")
    {
        const Opened opened = openSheet(kDemoData / "normal.csv");
        const CodeSource source = codeSourceOf(kDemoData / "normal.csv", *opened.book, *opened.model, ';');
        CHECK(source.delimiter == ';');
    }
}

TEST_CASE("codeFormatOf: by the extension, and databases by their header")
{
    CHECK(codeFormatOf(kDemoData / "normal.parquet", false) == CodeFormat::Parquet);
    CHECK(codeFormatOf(kDemoData / "normal.json", false) == CodeFormat::Json);
    CHECK(codeFormatOf(kDemoData / "normal.ndjson", false) == CodeFormat::NdJson);
    CHECK(codeFormatOf(kDemoData / "normal.jsonl.gz", false) == CodeFormat::NdJson);
    CHECK(codeFormatOf(kDemoData / "normal.xlsx", false) == CodeFormat::Excel);
    CHECK(codeFormatOf(kDemoData / "demo.sqlite3", false) == CodeFormat::Sqlite);
    CHECK(codeFormatOf(kDemoData / "demo.duckdb", false) == CodeFormat::DuckDb);
    CHECK(codeFormatOf(kDemoData / "normal.csv.gz", true) == CodeFormat::Text);

    // A ".db" file is told by its first bytes.
    const std::filesystem::path db = std::filesystem::temp_directory_path() / "analysis-code.db";
    std::filesystem::copy_file(kDemoData / "demo.duckdb", db, std::filesystem::copy_options::overwrite_existing);
    CHECK(codeFormatOf(db, false) == CodeFormat::DuckDb);
    std::filesystem::copy_file(kDemoData / "demo.sqlite3", db, std::filesystem::copy_options::overwrite_existing);
    CHECK(codeFormatOf(db, false) == CodeFormat::Sqlite);
    std::filesystem::remove(db);
}

TEST_CASE("analysisCode: reading the file")
{
    AnalysisCodeSpec spec;
    spec.source = normalSource();

    SECTION("CSV and TSV")
    {
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "path <- \"/data/normal.csv\"\ndf <- read_csv(path)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars),
                       "df = pl.read_csv(path, try_parse_dates=True, infer_schema_length=10000)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "df = pd.read_csv(path)\n"));

        spec.source.delimiter = '\t';
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "df <- read_tsv(path)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "separator=\"\\t\""));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "df = pd.read_csv(path, sep=\"\\t\")\n"));

        // Guessed by TableSkim: R and pandas guess too.
        spec.source.delimiter.reset();
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "df <- read_delim(path)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "# Set the separator"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "sep=None, engine=\"python\""));
    }
    SECTION("No header row, an encoding and meta lines")
    {
        spec.source.path = "/data/calls.vcf.gz";
        spec.source.delimiter = '\t';
        spec.source.header = false;
        spec.source.columns = {"A", "B"};
        spec.source.skip_meta_lines = true;
        spec.source.encoding = TextEncoding::Cp932;
        const std::string r = analysisCode(spec, CodeLanguage::R);
        CHECK(contains(r, "col_names = c(\"A\", \"B\")"));
        CHECK(contains(r, "comment = \"##\""));
        CHECK(contains(r, "locale = locale(encoding = \"CP932\")"));
        const std::string polars = analysisCode(spec, CodeLanguage::Polars);
        CHECK(contains(polars, "has_header=False"));
        CHECK(contains(polars, "new_columns=[\"A\", \"B\"]"));
        CHECK(contains(polars, "comment_prefix=\"##\""));
        CHECK(contains(polars, "encoding=\"cp932\""));
        const std::string pandas = analysisCode(spec, CodeLanguage::Pandas);
        CHECK(contains(pandas, "import gzip\nimport io\n"));
        CHECK(contains(pandas, "with gzip.open(path, \"rt\", encoding=\"cp932\") as file:\n"));
        CHECK(contains(pandas, "if not line.startswith(\"##\")"));
        CHECK(contains(pandas, "io.StringIO(text)"));
        CHECK(contains(pandas, "header=None"));
        CHECK(contains(pandas, "names=[\"A\", \"B\"]"));
    }
    SECTION("Compression polars does not read")
    {
        spec.source.path = "/data/normal.csv.xz";
        const std::string polars = analysisCode(spec, CodeLanguage::Polars);
        CHECK(contains(polars, "import lzma\n"));
        CHECK(contains(polars, "with lzma.open(path) as file:\n    data = file.read()\n"));
        CHECK(contains(polars, "pl.read_csv(data"));
    }
    SECTION("Excel")
    {
        spec.source.path = "/data/book.xlsx";
        spec.source.format = CodeFormat::Excel;
        spec.source.table = "Sheet 2";
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "library(readxl)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "read_excel(path, sheet = \"Sheet 2\")"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "pl.read_excel(path, sheet_name=\"Sheet 2\")"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "pd.read_excel(path, sheet_name=\"Sheet 2\")"));
    }
    SECTION("Databases")
    {
        spec.source.path = "/data/demo.duckdb";
        spec.source.format = CodeFormat::DuckDb;
        spec.source.table = "sales.orders";
        const std::string r = analysisCode(spec, CodeLanguage::R);
        CHECK(contains(r, "dbConnect(duckdb::duckdb(), path, read_only = TRUE)"));
        CHECK(contains(r, "dbGetQuery(con, 'SELECT * FROM \"sales\".\"orders\"')"));
        const std::string polars = analysisCode(spec, CodeLanguage::Polars);
        CHECK(contains(polars, "import duckdb\n"));
        CHECK(contains(polars, "df = con.sql('SELECT * FROM \"sales\".\"orders\"').pl()\n"));

        spec.source.format = CodeFormat::Sqlite;
        spec.source.table = "a\"b";
        const std::string pandas = analysisCode(spec, CodeLanguage::Pandas);
        CHECK(contains(pandas, "import sqlite3\n"));
        CHECK(contains(pandas, "pd.read_sql_query('SELECT * FROM \"a\"\"b\"', con)"));
    }
    SECTION("Parquet and JSON")
    {
        spec.source.format = CodeFormat::Parquet;
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "library(arrow)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "pl.read_parquet(path)"));
        spec.source.format = CodeFormat::NdJson;
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "pd.read_json(path, lines=True)"));
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "stream_in(file(path), verbose = FALSE)"));
    }
    SECTION("Strings are escaped")
    {
        spec.source.path = "C:\\data\\\"odd\".csv";
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "path <- 'C:\\\\data\\\\\"odd\".csv'"));
        spec.source.path = "it's \"x\".csv";
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "path = \"it's \\\"x\\\".csv\""));
    }
}

TEST_CASE("analysisCode: filters, sorts and hidden columns of the view")
{
    AnalysisCodeSpec spec;
    spec.source = normalSource();
    Filter filter;
    filter.conditions = {
        {AGE, FilterOp::GreaterEqual, "40"},
        {CITY, FilterOp::Contains, "OSA"},
        {NAME, FilterOp::NotEquals, "Taro"},
    };
    spec.view.filter = filter;
    spec.view.sort = {{AGE, false}, {NAME, true}};
    spec.hidden = {SALARY, LAST_LOGIN};

    const std::string r = analysisCode(spec, CodeLanguage::R);
    CHECK(contains(r, "  filter(\n    Age >= 40,\n    str_detect(City, fixed(\"OSA\", ignore_case = TRUE)),\n"
                      "    (is.na(Name) | Name != \"Taro\")\n  )\n"));
    CHECK(contains(r, "  arrange(desc(Age), Name)\n"));
    CHECK(contains(r, "  select(-c(Salary, `Last Login`))\n"));
    CHECK(contains(r, "print(df)\n"));

    const std::string polars = analysisCode(spec, CodeLanguage::Polars);
    CHECK(contains(polars, "pl.col(\"Age\") >= 40,\n"));
    CHECK(contains(polars, "pl.col(\"City\").str.to_lowercase().str.contains(\"osa\", literal=True),\n"));
    CHECK(contains(polars, "pl.col(\"Name\").ne_missing(\"Taro\"),\n"));
    CHECK(contains(polars, "df = df.sort([\"Age\", \"Name\"], descending=[True, False], nulls_last=True)\n"));
    CHECK(contains(polars, "df = df.drop([\"Salary\", \"Last Login\"])\n"));

    const std::string pandas = analysisCode(spec, CodeLanguage::Pandas);
    CHECK(contains(pandas, "df = df[\n    (df[\"Age\"] >= 40)\n"
                           "    & (df[\"City\"].str.contains(\"OSA\", case=False, regex=False, na=False))\n"
                           "    & (df[\"Name\"].fillna(\"\") != \"Taro\")\n]\n"));
    CHECK(contains(pandas, "df.sort_values([\"Age\", \"Name\"], ascending=[False, True], na_position=\"last\")"));
    CHECK(contains(pandas, "df = df.drop(columns=[\"Salary\", \"Last Login\"])\n"));

    SECTION("Any of the conditions, then the chart's narrowing")
    {
        spec.view.filter->combine = Filter::Combine::Or;
        spec.view.filter->conditions.resize(2);
        spec.view.filter->narrowing = {{SALARY, FilterOp::Less, "1e6"}};
        CHECK(contains(analysisCode(spec, CodeLanguage::R),
                       "    Age >= 40 |\n    str_detect(City, fixed(\"OSA\", ignore_case = TRUE)),\n"
                       "    Salary < 1000000\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars),
                       "    (pl.col(\"Age\") >= 40)\n    | (pl.col(\"City\")"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "    & (df[\"Salary\"] < 1000000)\n"));
    }
    SECTION("Values compared as the column's type")
    {
        spec.view.sort.clear();
        spec.hidden.clear();
        spec.view.filter->conditions = {
            {BIRTHDAY, FilterOp::Less, "1990-01-31"},
            {LAST_LOGIN, FilterOp::Equals, "2024-05-01"},
            {LAST_LOGIN, FilterOp::Greater, "2024-05-01 12:30"},
            {ACTIVE, FilterOp::Equals, "yes"},
            {CITY, FilterOp::Greater, "5"},     // text stays text
            {ID, FilterOp::Equals, "abc"},      // not a number: compared as text
            {NAME, FilterOp::Matches, "^s.*a$"},
            {CITY, FilterOp::IsEmpty, ""},
            {AGE, FilterOp::IsNotEmpty, ""},
            {NAME, FilterOp::IsNumber, ""},
            {NAME, FilterOp::IsDate, ""},
        };
        const std::string r2 = analysisCode(spec, CodeLanguage::R);
        CHECK(contains(r2, "Birthday < as.Date(\"1990-01-31\")"));
        CHECK(contains(r2, "as.Date(`Last Login`) == as.Date(\"2024-05-01\")"));
        CHECK(contains(r2, "`Last Login` > as.POSIXct(\"2024-05-01 12:30:00\", tz = \"UTC\")"));
        CHECK(contains(r2, "Active == TRUE"));
        CHECK(contains(r2, "City > \"5\""));
        CHECK(contains(r2, "as.character(ID) == \"abc\""));
        CHECK(contains(r2, "str_detect(Name, regex(\"^s.*a$\", ignore_case = TRUE))"));
        CHECK(contains(r2, "(is.na(City) | City == \"\")"));
        CHECK(contains(r2, "!is.na(Age)"));
        CHECK(contains(r2, "!is.na(suppressWarnings(as.numeric(Name)))"));
        CHECK(contains(r2, "!is.na(ymd_hms(Name, truncated = 3, quiet = TRUE))"));

        const std::string polars2 = analysisCode(spec, CodeLanguage::Polars);
        CHECK(contains(polars2, "pl.col(\"Birthday\") < pl.date(1990, 1, 31)"));
        CHECK(contains(polars2, "pl.col(\"Last Login\").dt.date() == pl.date(2024, 5, 1)"));
        CHECK(contains(polars2, "pl.col(\"Last Login\") > pl.datetime(2024, 5, 1, 12, 30, 0)"));
        CHECK(contains(polars2, "pl.col(\"Active\") == True"));
        CHECK(contains(polars2, "pl.col(\"ID\").cast(pl.String) == \"abc\""));
        CHECK(contains(polars2, "pl.col(\"Name\").str.contains(\"(?i)^s.*a$\")"));

        const std::string pandas2 = analysisCode(spec, CodeLanguage::Pandas);
        // Dates in text become dates first.
        CHECK(contains(pandas2, "df[\"Birthday\"] = pd.to_datetime(df[\"Birthday\"], format=\"ISO8601\")\n"));
        CHECK(contains(pandas2, "df[\"Last Login\"] = pd.to_datetime(df[\"Last Login\"], format=\"ISO8601\")\n"));
        CHECK(contains(pandas2, "(df[\"Birthday\"] < pd.Timestamp(\"1990-01-31\"))"));
        CHECK(contains(pandas2, "(df[\"Last Login\"].dt.normalize() == pd.Timestamp(\"2024-05-01\"))"));
        CHECK(contains(pandas2, "(df[\"Last Login\"] > pd.Timestamp(\"2024-05-01 12:30:00\"))"));
        CHECK(contains(pandas2, "(pd.to_numeric(df[\"Name\"], errors=\"coerce\").notna())"));
    }
}

TEST_CASE("analysisCode: Group By")
{
    AnalysisCodeSpec spec;
    spec.source = normalSource();
    spec.view.sort = {{AGE, true}}; // not needed for a Group By
    spec.analysis = GroupBySpec{.keys = {CITY, ACTIVE}, .values = {SALARY, NAME},
                                .aggregates = {Aggregate::Sum, Aggregate::StdDev, Aggregate::Distinct}};

    const std::string r = analysisCode(spec, CodeLanguage::R);
    CHECK(r.find("arrange(") == std::string::npos);
    CHECK(contains(r, "# Sums, means and standard deviations are left out for Name (not numbers)\n"));
    CHECK(contains(r, "result <- df |>\n  group_by(City, Active) |>\n  summarise(\n    count = n(),\n"
                      "    `Salary sum` = sum(Salary, na.rm = TRUE),\n"
                      "    `Salary std dev` = sd(Salary, na.rm = TRUE),\n"
                      "    `Salary distinct` = n_distinct(Salary, na.rm = TRUE),\n"
                      "    `Name distinct` = n_distinct(Name, na.rm = TRUE),\n"
                      "    .groups = \"drop\"\n  )\nprint(result)\n"));

    const std::string polars = analysisCode(spec, CodeLanguage::Polars);
    CHECK(contains(polars, "    df.group_by([\"City\", \"Active\"])\n    .agg(\n        pl.len().alias(\"count\"),\n"
                           "        pl.col(\"Salary\").sum().alias(\"Salary sum\"),\n"
                           "        pl.col(\"Salary\").std().alias(\"Salary std dev\"),\n"
                           "        pl.col(\"Salary\").drop_nulls().n_unique().alias(\"Salary distinct\"),\n"));
    CHECK(contains(polars, "    .sort([\"City\", \"Active\"], nulls_last=True)\n"));

    const std::string pandas = analysisCode(spec, CodeLanguage::Pandas);
    CHECK(contains(pandas, "df.groupby([\"City\", \"Active\"], dropna=False)\n"));
    CHECK(contains(pandas, "            \"count\": (\"City\", \"size\"),\n"
                           "            \"Salary sum\": (\"Salary\", \"sum\"),\n"
                           "            \"Salary std dev\": (\"Salary\", \"std\"),\n"
                           "            \"Salary distinct\": (\"Salary\", \"nunique\"),\n"
                           "            \"Name distinct\": (\"Name\", \"nunique\"),\n"));
    CHECK(contains(pandas, ".reset_index()\n"));

    SECTION("No keys: one group of every row")
    {
        std::get<GroupBySpec>(spec.analysis).keys.clear();
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "result <- df |>\n  summarise(\n    count = n(),\n"));
        CHECK(analysisCode(spec, CodeLanguage::R).find(".groups") == std::string::npos);
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "df.select("));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "\"Salary sum\": [df[\"Salary\"].sum()],\n"));
    }
}

TEST_CASE("analysisCode: histograms and scatter plots")
{
    AnalysisCodeSpec spec;
    spec.source = normalSource();
    spec.view.filter = Filter{.conditions = {{AGE, FilterOp::Less, "60"}}};

    SECTION("Histogram of numbers, stacked by a column")
    {
        spec.analysis = HistogramCodeSpec{.column = SALARY, .group_column = ACTIVE, .bins = 20};
        const std::string r = analysisCode(spec, CodeLanguage::R);
        CHECK(contains(r, "  filter(Age < 60)\n"));
        CHECK(contains(r, "p <- ggplot(df, aes(x = Salary, fill = factor(Active))) +\n  geom_histogram(bins = 20) +\n"));
        CHECK(contains(r, "labs(y = \"rows\", fill = \"Active\")"));
        const std::string polars = analysisCode(spec, CodeLanguage::Polars);
        CHECK(contains(polars, "from plotnine import aes, geom_histogram, ggplot, labs\n"));
        CHECK(contains(polars, "df = df.with_columns(pl.col(\"Active\").cast(pl.String))\n"));
        CHECK(contains(polars, "ggplot(df, aes(x=\"Salary\", fill=\"Active\"))\n    + geom_histogram(bins=20)"));
        CHECK(contains(polars, "p.show()\n"));
        const std::string pandas = analysisCode(spec, CodeLanguage::Pandas);
        CHECK(contains(pandas, "import matplotlib.pyplot as plt\nimport pandas as pd\n"));
        CHECK(contains(pandas, "df.groupby(\"Active\")[\"Salary\"]"));
        CHECK(contains(pandas, "stacked=True"));
        CHECK(contains(pandas, "plt.show()\n"));
    }
    SECTION("Histogram of text: the most frequent values")
    {
        spec.analysis = HistogramCodeSpec{.column = CITY};
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "count(City, sort = TRUE) |>\n  slice_head(n = 30)\n"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "value_counts(sort=True).head(30)"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "counts.plot.bar(ax=ax)"));
    }
    SECTION("Scatter plot coloured by a column")
    {
        spec.analysis = ScatterCodeSpec{.x_column = AGE, .y_column = SALARY, .color_column = CITY};
        CHECK(contains(analysisCode(spec, CodeLanguage::R),
                       "ggplot(df, aes(x = Age, y = Salary, colour = factor(City))) +\n  geom_point(alpha = 0.5)"));
        const std::string polars = analysisCode(spec, CodeLanguage::Polars);
        CHECK(polars.find("with_columns") == std::string::npos); // text is discrete already
        CHECK(contains(polars, "aes(x=\"Age\", y=\"Salary\", color=\"City\")"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "for key, rows in df.groupby(\"City\"):\n"));
    }
    SECTION("A text X makes box plots")
    {
        spec.analysis = ScatterCodeSpec{.x_column = CITY, .y_column = SALARY};
        CHECK(contains(analysisCode(spec, CodeLanguage::R), "geom_boxplot()"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Polars), "+ geom_boxplot()"));
        CHECK(contains(analysisCode(spec, CodeLanguage::Pandas), "df.boxplot(column=\"Salary\", by=\"City\", ax=ax)"));
    }
}

TEST_CASE("analysisCode: file names and languages")
{
    AnalysisCodeSpec spec;
    spec.source = normalSource();
    spec.source.path = "/data/sales.csv.gz";
    CHECK(analysisCodeFileName(spec, CodeLanguage::R) == "sales-view.R");
    spec.analysis = HistogramCodeSpec{};
    CHECK(analysisCodeFileName(spec, CodeLanguage::Polars) == "sales-histogram-polars.py");
    spec.analysis = GroupBySpec{};
    CHECK(analysisCodeFileName(spec, CodeLanguage::Pandas) == "sales-group-by-pandas.py");

    for (const CodeLanguage language : CODE_LANGUAGES)
    {
        CHECK(parseCodeLanguage(codeLanguageId(language)) == language);
        CHECK(codeLanguageFromFileName(analysisCodeFileName(spec, language)) == language);
    }
    CHECK(codeLanguageFromFileName("x.r") == CodeLanguage::R);
    CHECK_FALSE(codeLanguageFromFileName("x.py"));
    CHECK_FALSE(codeLanguageFromFileName("x.txt"));
    CHECK_FALSE(parseCodeLanguage("python"));
}

// Writes the code for the demo data to TABLESKIM_CODE_DIR, to run it with R
// and Python by hand, with the rows of TableSkim's view in "<case>.rows":
// after the filter, the code's `df` should have as many.
TEST_CASE("analysisCode: write runnable examples", "[.analysis-code-run]")
{
    const char *dir = std::getenv("TABLESKIM_CODE_DIR");
    REQUIRE(dir);
    const std::filesystem::path out = utf8Path(dir);
    std::filesystem::create_directories(out);

    struct Case
    {
        std::string name;
        std::filesystem::path file;
        std::string sheet;
        std::function<void(AnalysisCodeSpec &)> setup;
    };
    // normal.csv: ID, Name, Email, Age, City, Department, Salary, Birthday, Active
    const std::vector<Case> cases = {
        {"csv-view", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         {
             spec.view.filter = Filter{.conditions = {{3, FilterOp::GreaterEqual, "40"},
                                                      {4, FilterOp::Contains, "OSA"},
                                                      {1, FilterOp::NotEquals, "Taro"},
                                                      {7, FilterOp::Less, "1990-01-01"},
                                                      {8, FilterOp::Equals, "TRUE"}}};
             spec.view.sort = {{6, false}, {1, true}};
             spec.hidden = {2};
         }},
        {"csv-or", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         {
             spec.view.filter = Filter{.combine = Filter::Combine::Or,
                                       .conditions = {{1, FilterOp::Matches, "^s"}, {4, FilterOp::IsEmpty, ""}},
                                       .narrowing = {{6, FilterOp::IsNumber, ""}, {7, FilterOp::IsDate, ""}}};
         }},
        {"csv-group-by", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         {
             spec.analysis = GroupBySpec{.keys = {4, 8}, .values = {6, 1},
                                         .aggregates = {Aggregate::Count, Aggregate::Distinct, Aggregate::Sum,
                                                        Aggregate::Mean, Aggregate::Min, Aggregate::Max,
                                                        Aggregate::StdDev}};
         }},
        {"csv-group-all", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         { spec.analysis = GroupBySpec{.values = {6}, .aggregates = {Aggregate::Sum, Aggregate::Mean}}; }},
        {"csv-histogram", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         { spec.analysis = HistogramCodeSpec{.column = 6, .group_column = 8, .bins = 20}; }},
        {"csv-date-histogram", "normal.csv", "",
         [](AnalysisCodeSpec &spec) { spec.analysis = HistogramCodeSpec{.column = 7}; }},
        {"csv-text-histogram", "normal.csv", "",
         [](AnalysisCodeSpec &spec) { spec.analysis = HistogramCodeSpec{.column = 4}; }},
        {"csv-scatter", "normal.csv", "",
         [](AnalysisCodeSpec &spec)
         { spec.analysis = ScatterCodeSpec{.x_column = 3, .y_column = 6, .color_column = 8}; }},
        {"csv-box", "normal.csv", "",
         [](AnalysisCodeSpec &spec) { spec.analysis = ScatterCodeSpec{.x_column = 4, .y_column = 6}; }},
        {"cp932", "normal-cp932.csv.gz", "",
         [](AnalysisCodeSpec &spec) { spec.view.filter = Filter{.conditions = {{3, FilterOp::Greater, "30"}}}; }},
        {"bz2", "normal.csv.bz2", "", [](AnalysisCodeSpec &) {}},
        {"xlsx", "normal.xlsx", "", [](AnalysisCodeSpec &spec)
         { spec.view.filter = Filter{.conditions = {{7, FilterOp::GreaterEqual, "1990-01-01"}}}; }},
        {"parquet", "normal.parquet", "", [](AnalysisCodeSpec &spec)
         { spec.analysis = ScatterCodeSpec{.x_column = 7, .y_column = 6}; }},
        {"ndjson", "normal.ndjson", "", [](AnalysisCodeSpec &spec)
         { spec.analysis = GroupBySpec{.keys = {4}, .values = {3}, .aggregates = {Aggregate::Mean}}; }},
        {"sqlite", "demo.sqlite3", "", [](AnalysisCodeSpec &) {}},
        {"duckdb", "demo.duckdb", "", [](AnalysisCodeSpec &) {}},
        {"dates", "dates.csv", "", [](AnalysisCodeSpec &spec)
         {
             spec.view.filter = Filter{.conditions = {{2, FilterOp::Greater, "2000-01-01 12:00"},
                                                      {1, FilterOp::LessEqual, "2030-01-01"}}};
         }},
    };
    for (const Case &c : cases)
    {
        const Opened opened = openSheet(kDemoData / c.file, c.sheet);
        AnalysisCodeSpec spec;
        spec.source = codeSourceOf(kDemoData / c.file, *opened.book, *opened.model);
        c.setup(spec);
        for (const CodeLanguage language : CODE_LANGUAGES)
        {
            std::ofstream file(out / (c.name + "-" + codeLanguageId(language) + codeLanguageExtension(language)),
                               std::ios::binary);
            file << analysisCode(spec, language);
        }
        // The rows of TableSkim's view, which the code's `df` should have.
        opened.model->setView(spec.view);
        REQUIRE(waitFor([&]
                        { return opened.model->view() == spec.view && opened.model->rowCount().isExact(); }));
        std::ofstream(out / (c.name + ".rows"), std::ios::binary) << opened.model->rowCount().value << "\n";
    }
}
