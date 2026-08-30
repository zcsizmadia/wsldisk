"""Tests for the coverage gate.

check-coverage.py is what stops undertested code from shipping, so a bug that
makes it pass everything would be invisible. These run in the lint job:

    python -m pytest scripts/tests
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).resolve().parents[1] / "check-coverage.py"
_SPEC = importlib.util.spec_from_file_location("check_coverage", _MODULE_PATH)
assert _SPEC and _SPEC.loader
check_coverage = importlib.util.module_from_spec(_SPEC)
sys.modules["check_coverage"] = check_coverage
_SPEC.loader.exec_module(check_coverage)


def lcov(body: str, path: str = "src/lib/example.cpp") -> str:
    return f"SF:{path}\n{body.strip()}\nend_of_record\n"


def no_exclusions(_path: str):
    return check_coverage.SourceExclusions()


def parse(text: str, exclusions=no_exclusions):
    return check_coverage.parse_lcov(text, exclusions_for=exclusions)


class TestParsing:
    def test_counts_lines_branches_and_functions(self):
        files, _ = parse(
            lcov(
                """
                FN:5,covered
                FNDA:3,covered
                FN:9,never_called
                FNDA:0,never_called
                DA:5,3
                DA:6,0
                BRDA:5,0,0,3
                BRDA:5,0,1,0
                """
            )
        )
        assert len(files) == 1
        file = files[0]
        assert (file.lines_found, file.lines_hit) == (2, 1)
        assert (file.branches_found, file.branches_hit) == (2, 1)
        assert (file.functions_found, file.functions_hit) == (2, 1)
        assert file.uncovered_lines == [6]
        assert file.uncovered_branches == [5]
        assert file.uncovered_functions == [(9, "never_called")]

    def test_a_function_announced_without_a_hit_record_counts_as_uncovered(self):
        # llvm-cov omits FNDA for a function that was never entered.
        files, _ = parse(lcov("FN:5,orphan\nDA:5,0"))
        assert files[0].functions_found == 1
        assert files[0].functions_hit == 0

    def test_a_branch_llvm_could_not_instrument_counts_as_untaken(self):
        files, _ = parse(lcov("DA:5,1\nBRDA:5,0,0,-"))
        assert (files[0].branches_found, files[0].branches_hit) == (1, 0)

    def test_an_empty_file_is_fully_covered(self):
        empty = check_coverage.FileCoverage(path="x")
        assert empty.line_percentage == 100.0
        assert empty.branch_percentage == 100.0
        assert empty.function_percentage == 100.0

    def test_multiple_records_are_kept_separate(self):
        files, _ = parse(lcov("DA:1,1", "a.cpp") + lcov("DA:1,0", "b.cpp"))
        assert [f.path for f in files] == ["a.cpp", "b.cpp"]
        assert files[0].lines_hit == 1
        assert files[1].lines_hit == 0

    def test_records_before_any_source_file_are_ignored(self):
        files, _ = parse("DA:1,0\n" + lcov("DA:1,1"))
        assert len(files) == 1
        assert files[0].lines_found == 1


class TestExclusions:
    def write(self, tmp_path: Path, text: str) -> str:
        source = tmp_path / "example.cpp"
        source.write_text(text, encoding="utf-8")
        return str(source)

    def test_excl_line_drops_the_line_and_its_branches(self, tmp_path: Path):
        path = self.write(
            tmp_path,
            "int f() {\n"
            "    return 1;\n"
            "    return 2;  // LCOV_EXCL_LINE\n"
            "}\n",
        )
        files, applied = check_coverage.parse_lcov(lcov("DA:2,1\nDA:3,0\nBRDA:3,0,0,0", path))
        assert files[0].lines_found == 1
        assert files[0].branches_found == 0
        assert [e.line for e in applied] == [3]

    def test_excl_br_line_drops_only_the_branches(self, tmp_path: Path):
        path = self.write(tmp_path, "a\nb  // LCOV_EXCL_BR_LINE\n")
        files, _ = check_coverage.parse_lcov(lcov("DA:2,1\nBRDA:2,0,0,0", path))
        assert files[0].lines_found == 1
        assert files[0].branches_found == 0

    def test_excl_start_stop_drops_the_whole_block(self, tmp_path: Path):
        path = self.write(
            tmp_path,
            "keep\n"
            "// LCOV_EXCL_START\n"
            "dropped\n"
            "dropped\n"
            "// LCOV_EXCL_STOP\n"
            "keep\n",
        )
        files, _ = check_coverage.parse_lcov(
            lcov("DA:1,1\nDA:3,0\nDA:4,0\nDA:6,1", path)
        )
        assert files[0].lines_found == 2
        assert files[0].lines_hit == 2

    def test_a_function_on_an_excluded_line_is_not_counted(self, tmp_path: Path):
        path = self.write(tmp_path, "a\nvoid f() {}  // LCOV_EXCL_LINE\n")
        files, _ = check_coverage.parse_lcov(lcov("FN:2,f\nFNDA:0,f\nDA:2,0", path))
        assert files[0].functions_found == 0

    def test_an_unterminated_block_is_an_error(self, tmp_path: Path):
        path = self.write(tmp_path, "// LCOV_EXCL_START\ncode\n")
        with pytest.raises(ValueError, match="without a matching"):
            check_coverage.parse_lcov(lcov("DA:2,0", path))

    def test_a_source_file_that_is_gone_simply_has_no_exclusions(self, tmp_path: Path):
        missing = str(tmp_path / "not-there.cpp")
        files, applied = check_coverage.parse_lcov(lcov("DA:1,0", missing))
        assert files[0].lines_found == 1
        assert applied == []


class TestDefaultedSpecialMembers:
    def write(self, tmp_path: Path, text: str) -> str:
        source = tmp_path / "example.h"
        source.write_text(text, encoding="utf-8")
        return str(source)

    def test_a_defaulted_destructor_is_not_counted(self, tmp_path: Path):
        path = self.write(tmp_path, "class A {\n    virtual ~A() = default;\n};\n")
        files, _ = check_coverage.parse_lcov(lcov("FN:2,dtor\nFNDA:0,dtor\nDA:2,0", path))
        assert files[0].functions_found == 0
        assert files[0].lines_found == 0
        assert files[0].defaulted_functions == 1

    def test_spacing_around_default_does_not_matter(self, tmp_path: Path):
        path = self.write(tmp_path, "class A {\n    A()=default;\n};\n")
        files, _ = check_coverage.parse_lcov(lcov("FN:2,ctor\nFNDA:0,ctor\nDA:2,0", path))
        assert files[0].functions_found == 0

    def test_a_deleted_member_is_not_treated_as_defaulted(self, tmp_path: Path):
        # `= delete` has no body to cover either, but llvm-cov never reports one,
        # so a rule that swallowed it would only hide real gaps.
        path = self.write(tmp_path, "class A {\n    A(const A&) = delete;\n};\n")
        files, _ = check_coverage.parse_lcov(lcov("FN:2,copy\nFNDA:0,copy\nDA:2,0", path))
        assert files[0].functions_found == 1

    def test_a_real_body_on_the_same_line_still_counts(self, tmp_path: Path):
        path = self.write(tmp_path, "class A {\n    int f() { return by_default(); }\n};\n")
        files, _ = check_coverage.parse_lcov(lcov("FN:2,f\nFNDA:0,f\nDA:2,0", path))
        assert files[0].functions_found == 1

    def test_defaulted_members_do_not_count_against_the_exclusion_cap(self, tmp_path: Path):
        path = self.write(tmp_path, "class A {\n    ~A() = default;\n};\n")
        _, applied = check_coverage.parse_lcov(lcov("FN:2,dtor\nFNDA:0,dtor\nDA:2,0", path))
        assert applied == []


class TestGate:
    def run(self, tmp_path: Path, body: str, *args: str) -> int:
        report = tmp_path / "coverage.lcov"
        report.write_text(lcov(body), encoding="utf-8")
        return check_coverage.main([str(report), *args])

    def test_full_coverage_passes(self, tmp_path: Path):
        assert self.run(tmp_path, "FN:1,f\nFNDA:1,f\nDA:1,1\nBRDA:1,0,0,1") == 0

    def test_a_missing_line_fails(self, tmp_path: Path):
        assert self.run(tmp_path, "FN:1,f\nFNDA:1,f\nDA:1,1\nDA:2,0") == 1

    def test_a_missing_branch_fails(self, tmp_path: Path):
        assert self.run(tmp_path, "FN:1,f\nFNDA:1,f\nDA:1,1\nBRDA:1,0,0,0") == 1

    def test_an_uncalled_function_fails(self, tmp_path: Path):
        assert self.run(tmp_path, "FN:1,f\nFNDA:0,f\nDA:1,1") == 1

    def test_a_lower_threshold_can_be_requested(self, tmp_path: Path):
        assert self.run(tmp_path, "DA:1,1\nDA:2,0", "--lines", "50", "--functions", "0") == 0

    def test_a_missing_report_is_an_error(self, tmp_path: Path):
        assert check_coverage.main([str(tmp_path / "nope.lcov")]) == 2

    def test_an_empty_report_is_an_error(self, tmp_path: Path):
        report = tmp_path / "coverage.lcov"
        report.write_text("", encoding="utf-8")
        assert check_coverage.main([str(report)]) == 2

    def test_too_many_exclusions_fail_even_at_full_coverage(self, tmp_path: Path):
        source = tmp_path / "example.cpp"
        source.write_text("x  // LCOV_EXCL_LINE\n" * 3, encoding="utf-8")
        report = tmp_path / "coverage.lcov"
        report.write_text(lcov("DA:1,0\nDA:2,0\nDA:3,0", str(source)), encoding="utf-8")
        assert check_coverage.main([str(report), "--max-exclusions", "2"]) == 1
        assert check_coverage.main([str(report), "--max-exclusions", "3"]) == 0


def test_demangle_shortens_an_msvc_name():
    assert check_coverage.demangle("??1IFileSystem@wsldisk@@UEAA@XZ") == "wsldisk::IFileSystem"
    assert check_coverage.demangle("plain_name") == "plain_name"
