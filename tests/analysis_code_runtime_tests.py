"""Execute the generated regression examples with R and both Python writers.

TABLESKIM_CODE_DIR=/tmp/code build/debug/tableskim-tests '[.analysis-code-regressions]'
python tests/analysis_code_runtime_tests.py --code-dir /tmp/code --r /path/to/R

Requires pandas, matplotlib, polars, plotnine, pyarrow, and R with tidyverse.
R is the interpreter (accepting --file), rather than Rscript.
"""

import argparse
import gzip
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

os.environ["MPLBACKEND"] = "Agg"

import matplotlib.pyplot as plt


class GeneratedCodeTest(unittest.TestCase):
    code_dir: Path
    r_command: str

    def execute(self, name, language):
        self.addCleanup(plt.close, "all")
        return runpy.run_path(str(self.code_dir / f"{name}-{language}.py"))

    def test_group_keys_and_values_survive_count_names(self):
        for language in ("polars", "pandas"):
            with self.subTest(language=language):
                result = self.execute("group-key", language)["result"]
                if language == "polars":
                    result = result.to_pandas()
                result = result.sort_values("count")
                self.assertEqual(result["count"].tolist(), [1, 2])
                self.assertEqual(result["count_3"].tolist(), [2, 1])
                self.assertEqual(result["value sum"].tolist(), [30, 30])
                result = self.execute("group-value", language)["result"]
                if language == "polars":
                    result = result.to_pandas()
                self.assertEqual(result["count sum"].tolist(), [4])
                self.assertEqual(result["count_3"].tolist(), [3])

    def test_bgz_with_and_without_metadata_filter(self):
        for language in ("polars", "pandas"):
            for name in ("bgz", "bgz-meta"):
                with self.subTest(language=language, name=name):
                    frame = self.execute(name, language)["df"]
                    self.assertEqual(len(frame), 3)
                    self.assertEqual(frame["value"].tolist() if language == "pandas"
                                     else frame["value"].to_list(), [10, 20, 30])

    def test_polars_colour_keeps_numeric_axes_and_input_columns(self):
        histogram = self.execute("numeric-histogram", "polars")
        self.assertEqual(histogram["df"]["count"].to_list(), [10, 20, 100])
        self.assertEqual(histogram["df"]["count group"].to_list(), [99, 99, 99])
        self.assertGreater(histogram["p"].draw().axes[0].get_xlim()[1], 90)
        for column in (0, 3):
            with self.subTest(colour_column=column):
                plot = self.execute(f"scatter-{column}", "polars")["p"]
                plot.draw()
                points = plot._build_objs.layers[0].data
                self.assertEqual(sorted(points["x"]), [10, 20, 100])
                self.assertEqual(sorted(points["y"]), [3, 5, 7])

    def test_categorical_stacks_draw_parts_and_keep_top_categories(self):
        for language in ("polars", "pandas"):
            for prefix in ("stack", "self-stack"):
                for column in (0, 1):
                    with self.subTest(language=language, prefix=prefix, column=column):
                        namespace = self.execute(f"{prefix}-{column}", language)
                        if language == "polars":
                            plot = namespace["p"]
                            plot.draw()
                            bars = plot._build_objs.layers[0].data
                            self.assertGreaterEqual(bars["fill"].nunique(), 2)
                            total = (bars["ymax"] - bars["ymin"]).sum()
                        else:
                            axes = namespace["ax"]
                            self.assertGreaterEqual(len(axes.containers), 2)
                            total = sum(bar.get_height() for bar in axes.patches)
                        self.assertEqual(total, 4 if column == 0 else 5)

    def test_r_grouping_compression_and_actual_stack_rendering(self):
        # source() runs the emitted scripts unchanged, including print(p).
        script = r'''
args <- commandArgs(trailingOnly = TRUE)
root <- args[[1]]
pdf(args[[2]])
run <- function(name) {
  env <- new.env()
  source(file.path(root, paste0(name, "-r.R")), local = env)
  env
}
cat(R.version.string, "\n")
key <- run("group-key")$result
stopifnot(identical(as.numeric(key$count), c(1, 2)),
          identical(as.numeric(key$count_3), c(2, 1)),
          identical(as.numeric(key[["value sum"]]), c(30, 30)))
value <- run("group-value")$result
stopifnot(value[["count sum"]] == 4, value$count_3 == 3)
for (name in c("bgz", "bgz-meta")) stopifnot(nrow(run(name)$df) == 3)
for (prefix in c("stack", "self-stack")) {
  for (column in 0:1) {
    env <- run(paste0(prefix, "-", column))
    bars <- ggplot2::ggplot_build(env$p)$data[[1]]
    stopifnot(length(unique(bars$fill)) >= 2,
              sum(bars$ymax - bars$ymin) == if (column == 0) 4 else 5)
  }
}
dev.off()
cat("R grouping, BGZF reading and four stack plots passed\n")
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "verify.R"
            path.write_text(script, encoding="utf-8")
            result = subprocess.run(
                [self.r_command, "--vanilla", "--slave", f"--file={path}",
                 "--args", str(self.code_dir), str(Path(directory) / "plots.pdf")],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--code-dir", required=True, type=Path)
    parser.add_argument("--r", default=shutil.which("R"))
    args = parser.parse_args()
    if not args.r:
        parser.error("R is required; supply --r /path/to/R")
    GeneratedCodeTest.code_dir = args.code_dir.resolve()
    GeneratedCodeTest.r_command = args.r
    source = GeneratedCodeTest.code_dir / "counts.csv"
    source.with_suffix(".csv.bgz").write_bytes(gzip.compress(source.read_bytes()))
    unittest.main(argv=[__file__])
