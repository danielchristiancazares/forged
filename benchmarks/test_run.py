import unittest

from benchmarks.run import (
    parse_perf_archive_fallbacks,
    parse_perf_counters,
    parse_perf_timers,
)


class PerfParsingTest(unittest.TestCase):
    def test_parse_perf_timers_aggregates_repeated_names(self) -> None:
        stdout = """     User   System     Real  Name
    0.500    0.164    0.509  all
    0.140    0.080    0.252    read_input_files
    0.010    0.002    0.030      index_archive
    0.020    0.003    0.040      index_archive
"""

        rows, totals = parse_perf_timers(stdout)

        self.assertEqual(
            rows,
            [
                {"name": "all", "depth": 0, "user_s": 0.500, "sys_s": 0.164, "real_s": 0.509},
                {
                    "name": "read_input_files",
                    "depth": 1,
                    "user_s": 0.140,
                    "sys_s": 0.080,
                    "real_s": 0.252,
                },
                {"name": "index_archive", "depth": 2, "user_s": 0.010, "sys_s": 0.002, "real_s": 0.030},
                {"name": "index_archive", "depth": 2, "user_s": 0.020, "sys_s": 0.003, "real_s": 0.040},
            ],
        )
        self.assertAlmostEqual(totals["index_archive"], 0.070)

    def test_parse_perf_counters_reads_name_value_pairs(self) -> None:
        stdout = """
 num_archives_indexed=12
 num_archive_members_indexed=345
 num_archive_members_extracted=67
"""

        self.assertEqual(
            parse_perf_counters(stdout),
            {
                "num_archives_indexed": 12,
                "num_archive_members_indexed": 345,
                "num_archive_members_extracted": 67,
            },
        )

    def test_parse_perf_output_ignores_non_perf_lines(self) -> None:
        stdout = """warning: ignored
     User   System     Real  Name
    0.100    0.010    0.120  all
random text
 num_archives_eager_fallback=3
"""

        rows, totals = parse_perf_timers(stdout)
        counters = parse_perf_counters(stdout)

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["name"], "all")
        self.assertAlmostEqual(totals["all"], 0.120)
        self.assertEqual(counters["num_archives_eager_fallback"], 3)

    def test_parse_archive_fallbacks_reads_reason_and_path(self) -> None:
        stdout = """archive_fallback=no_darwin_symtab\t/tmp/libfoo.a
archive_fallback=empty_darwin_symtab\t/tmp/libbar.a
"""

        self.assertEqual(
            parse_perf_archive_fallbacks(stdout),
            [
                {"reason": "no_darwin_symtab", "path": "/tmp/libfoo.a"},
                {"reason": "empty_darwin_symtab", "path": "/tmp/libbar.a"},
            ],
        )


if __name__ == "__main__":
    unittest.main()
