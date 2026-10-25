"""End-to-end tests of the terminal UI (Linux only).

Each test runs `tableskim -t` in a pseudo-terminal (tui_harness.Tui) and
waits for text on the screen, never for fixed periods. Run by ctest as
tableskim-tui-tests, or by hand:

    python3 tests/tui/test_tui.py --tableskim build/debug/tableskim \
        --demo-data demo/data [unittest options, e.g. -k histogram]

Needs pyte (python3-pyte, or `pip install -r tests/tui/requirements.txt`).
"""

import argparse
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_harness import Tui  # noqa: E402

TABLESKIM = None
DEMO_DATA = None


def status(tui):
    """The status line, at the bottom of the screen."""
    return tui.line(tui.lines - 1)


class TuiTestCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.mkdtemp(prefix="tableskim-tui-test-")
        self.addCleanup(shutil.rmtree, self.temp, ignore_errors=True)

    def run_tui(self, path, *args):
        tui = Tui(TABLESKIM, path, args)
        self.addCleanup(tui.close)
        tui.start()
        return tui

    def demo(self, name):
        return os.path.join(DEMO_DATA, name)

    def temp_file(self, name, text):
        path = os.path.join(self.temp, name)
        with open(path, "w", encoding="utf-8", newline="") as out:
            out.write(text)
        return path


class OpenAndQuitTest(TuiTestCase):
    def test_shows_the_file_and_quits(self):
        tui = self.run_tui(self.demo("normal.csv"))
        tui.wait_text("55 rows × 9 cols")
        self.assertIn("Sakura Brown", tui.text())
        tui.send("q")
        self.assertEqual(tui.wait_exit(), 0)


class HiddenColumnsTest(TuiTestCase):
    def test_histogram_columns_skip_hidden_ones(self):
        tui = self.run_tui(self.demo("normal.csv"))
        tui.wait_text("55 rows")
        tui.send("H")  # ID, Name, Email, ...
        tui.wait_text("54 rows")
        tui.send("l", "x")  # hides Name
        tui.wait_text("1 hidden")
        tui.send("0", "B")
        tui.wait_text("Histogram: ID")
        tui.send("l")
        tui.wait_text("Histogram: Email")
        tui.send("h")
        tui.wait_text("Histogram: ID")

    def test_density_columns_skip_hidden_ones(self):
        tui = self.run_tui(self.demo("normal.parquet"))
        tui.wait_text("54 rows")
        tui.send("l", "l", "l", "l", "x")  # hides City, after Age
        tui.wait_text("1 hidden")
        tui.send("0", "M")
        tui.wait_text("Density: Age × ID")
        tui.send("x")
        tui.wait_text("Box plot: Department × ID")
        tui.send("X")
        tui.wait_text("Density: Age × ID")


class SqlResultTest(TuiTestCase):
    def run_sql(self, tui, sql):
        tui.send(":")
        tui.wait_text("Enter: run")
        tui.type(sql)
        tui.send("enter")
        tui.wait_text('The result is shown in the sheet "SQL Resul')

    def test_a_new_result_after_a_reopen_has_a_view_of_its_own(self):
        tui = self.run_tui(self.demo("normal.parquet"))
        tui.wait_text("54 rows")
        self.run_sql(tui, "SELECT * FROM normal WHERE Age < 30")
        tui.wait_text("13 rows × 9 cols")
        tui.send("=")  # ID = 13
        tui.wait_text('filter: ID = "13"')
        # Reopened on the file's sheet: the result's view waits to be restored.
        tui.send("]")
        tui.wait_text("54 rows × 9 cols")
        tui.send("r")
        tui.wait_text("Reverted")
        self.run_sql(tui, "SELECT * FROM normal WHERE Age >= 30")
        text = tui.wait_text("40 rows × 9 cols")
        self.assertNotIn("filter:", status(tui), text)

    def test_a_result_goes_before_the_group_by_sheet(self):
        tui = self.run_tui(self.demo("normal.parquet"))
        tui.wait_text("54 rows")
        tui.send("l", "l", "l", "l", "A")  # City, chosen at first
        tui.wait_text("Enter: group")
        tui.send("enter")
        tui.wait_text('12 groups in the sheet "Group By"')
        self.run_sql(tui, "SELECT 1 AS one")
        tui.wait_text("1 row × 1 cols")
        self.assertEqual(tui.line(0), " normal.parquet │ SQL Result │ Group By │")
        # The Group By sheet keeps its rows.
        tui.send("]")
        tui.wait_text("12 rows × 2 cols")
        self.assertIn("Fukuoka", tui.text())

        # Reopened on the Group By sheet: the query runs again and its result
        # is inserted before it, while the Group By stays shown.
        tui.send("r")
        tui.wait_text("Reverted")
        tui.wait_for(lambda text: text.splitlines()[0] == " normal.parquet │ SQL Result │ Group By │",
                     what="the sheets of the query and the Group By")
        tui.settle()
        self.assertIn("normal.parquet │ Group By │", status(tui))
        self.assertIn("12 rows × 2 cols", status(tui))
        tui.send("[")
        tui.wait_text("normal.parquet │ SQL Result │ R")
        self.assertIn("1 row × 1 cols", status(tui))


class EditingTest(TuiTestCase):
    def edit_first_value(self, tui, value):
        tui.send("I")
        tui.wait_text("EDIT")
        tui.send("j", "i")
        tui.wait_text("Edit R2C1")
        tui.send("ctrl+u")
        tui.type(value)
        tui.send("enter")
        tui.wait_text("*t.csv")

    def test_edits_are_saved(self):
        path = self.temp_file("t.csv", "a,b\n1,x\n2,y\n")
        tui = self.run_tui(path)
        tui.wait_text("3 rows")
        self.edit_first_value(tui, "42")
        tui.send("ctrl+s")
        tui.wait_text("Saved t.csv")
        with open(path, encoding="utf-8", newline="") as saved:
            self.assertEqual(saved.read(), "a,b\n42,x\n2,y\n")
        # Reopened after saving, still in edit mode.
        self.assertIn(" EDIT  t.csv │", status(tui))
        self.assertIn("42", tui.text())

    def test_a_conversion_does_not_open_over_unsaved_edits(self):
        path = self.temp_file("t.csv", "a,b\n1,x\n2,y\n")
        tui = self.run_tui(path)
        tui.wait_text("3 rows")
        self.edit_first_value(tui, "42")
        tui.send("P")
        tui.wait_text("not opened")
        self.assertTrue(os.path.exists(os.path.join(self.temp, "t.parquet")))
        # The edits are still there, unsaved.
        self.assertIn("*t.csv", status(tui))
        self.assertIn("42", tui.text())
        tui.send("ctrl+s")
        tui.wait_text("Saved t.csv")
        with open(path, encoding="utf-8", newline="") as saved:
            self.assertEqual(saved.read(), "a,b\n42,x\n2,y\n")


class FollowTest(TuiTestCase):
    def test_appended_rows_are_shown(self):
        path = self.temp_file("log.csv", "time,level\n12:00,INFO\n")
        tui = self.run_tui(path)
        tui.wait_text("2 rows")
        tui.send("T")
        tui.wait_text("follow")
        with open(path, "a", encoding="utf-8", newline="") as out:
            out.write("12:01,WARN\n12:02,ERROR\n")
        tui.wait_text("4 rows")
        self.assertIn("ERROR", tui.text())


def main():
    global TABLESKIM, DEMO_DATA
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--tableskim", required=True)
    parser.add_argument("--demo-data", required=True)
    args, rest = parser.parse_known_args()
    TABLESKIM = os.path.abspath(args.tableskim)
    DEMO_DATA = os.path.abspath(args.demo_data)
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
