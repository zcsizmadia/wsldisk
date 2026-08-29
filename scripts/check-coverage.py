#!/usr/bin/env python3
"""Enforce the coverage thresholds from docs/TESTING.md against an lcov report.

Reads the lcov file produced by `llvm-cov export -format=lcov`, prints a
per-file summary, and exits non-zero if any of the line / branch / function
percentages fall below the requested threshold.

Uncovered lines are also emitted as GitHub Actions workflow annotations when
running under CI, so a failing gate points at the exact source lines in the PR
diff rather than at a percentage.

Usage:
    python scripts/check-coverage.py build/x64-coverage/coverage.lcov \\
        --lines 100 --branches 100 --functions 100
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class FileCoverage:
    """Counters for one source file, in lcov's vocabulary."""

    path: str
    lines_found: int = 0
    lines_hit: int = 0
    branches_found: int = 0
    branches_hit: int = 0
    functions_found: int = 0
    functions_hit: int = 0
    uncovered_lines: list[int] = field(default_factory=list)
    uncovered_functions: list[str] = field(default_factory=list)

    def percentage(self, hit: int, found: int) -> float:
        # A file with nothing to cover is fully covered; that is how lcov and
        # every downstream tool reads it.
        return 100.0 if found == 0 else 100.0 * hit / found

    @property
    def line_percentage(self) -> float:
        return self.percentage(self.lines_hit, self.lines_found)

    @property
    def branch_percentage(self) -> float:
        return self.percentage(self.branches_hit, self.branches_found)

    @property
    def function_percentage(self) -> float:
        return self.percentage(self.functions_hit, self.functions_found)


def parse_lcov(text: str) -> list[FileCoverage]:
    """Parses the subset of lcov that llvm-cov emits."""
    files: list[FileCoverage] = []
    current: FileCoverage | None = None
    # Function names are announced by FN: before their hit count arrives in FNDA:.
    function_lines: dict[str, int] = {}

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("SF:"):
            current = FileCoverage(path=line[3:])
            function_lines = {}
        elif current is None:
            continue
        elif line.startswith("DA:"):
            number, _, count = line[3:].partition(",")
            current.lines_found += 1
            if int(count) > 0:
                current.lines_hit += 1
            else:
                current.uncovered_lines.append(int(number))
        elif line.startswith("FN:"):
            _, _, name = line[3:].partition(",")
            function_lines[name] = 0
        elif line.startswith("FNDA:"):
            count, _, name = line[5:].partition(",")
            function_lines[name] = int(count)
        elif line.startswith("BRDA:"):
            parts = line[5:].split(",")
            taken = parts[-1]
            current.branches_found += 1
            if taken != "-" and int(taken) > 0:
                current.branches_hit += 1
        elif line == "end_of_record":
            current.functions_found = len(function_lines)
            current.functions_hit = sum(1 for count in function_lines.values() if count > 0)
            current.uncovered_functions = sorted(
                name for name, count in function_lines.items() if count == 0
            )
            files.append(current)
            current = None

    return files


def annotate(path: str, line: int, message: str) -> None:
    """Emits a GitHub Actions annotation, or a plain line when running locally."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::error file={path},line={line}::{message}")
    else:
        print(f"  {path}:{line}: {message}")


def totals(files: list[FileCoverage]) -> FileCoverage:
    total = FileCoverage(path="TOTAL")
    for file in files:
        total.lines_found += file.lines_found
        total.lines_hit += file.lines_hit
        total.branches_found += file.branches_found
        total.branches_hit += file.branches_hit
        total.functions_found += file.functions_found
        total.functions_hit += file.functions_hit
    return total


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lcov", type=Path, help="path to the lcov report")
    parser.add_argument("--lines", type=float, default=100.0, help="minimum line coverage")
    parser.add_argument("--branches", type=float, default=100.0, help="minimum branch coverage")
    parser.add_argument("--functions", type=float, default=100.0, help="minimum function coverage")
    parser.add_argument(
        "--max-annotations",
        type=int,
        default=50,
        help="stop annotating uncovered lines after this many",
    )
    args = parser.parse_args(argv)

    if not args.lcov.is_file():
        print(f"error: no lcov report at {args.lcov}", file=sys.stderr)
        return 2

    files = parse_lcov(args.lcov.read_text(encoding="utf-8"))
    if not files:
        print(f"error: {args.lcov} contains no records", file=sys.stderr)
        return 2

    name_width = max(len(Path(file.path).as_posix()) for file in files)
    name_width = min(max(name_width, 20), 80)
    header = f"{'file':<{name_width}}  {'lines':>8}  {'branches':>9}  {'functions':>10}"
    print(header)
    print("-" * len(header))

    annotations = 0
    for file in sorted(files, key=lambda f: f.path):
        display = Path(file.path).as_posix()
        print(
            f"{display[-name_width:]:<{name_width}}  "
            f"{file.line_percentage:7.2f}%  "
            f"{file.branch_percentage:8.2f}%  "
            f"{file.function_percentage:9.2f}%"
        )
        for line in file.uncovered_lines:
            if annotations >= args.max_annotations:
                break
            annotate(display, line, "line not covered by any test")
            annotations += 1
        for function in file.uncovered_functions:
            if annotations >= args.max_annotations:
                break
            annotate(display, 1, f"function never called by a test: {function}")
            annotations += 1

    total = totals(files)
    print("-" * len(header))
    print(
        f"{'TOTAL':<{name_width}}  "
        f"{total.line_percentage:7.2f}%  "
        f"{total.branch_percentage:8.2f}%  "
        f"{total.function_percentage:9.2f}%"
    )

    failures = []
    for label, actual, required in (
        ("line", total.line_percentage, args.lines),
        ("branch", total.branch_percentage, args.branches),
        ("function", total.function_percentage, args.functions),
    ):
        if actual + 1e-9 < required:
            failures.append(f"{label} coverage {actual:.2f}% is below the required {required:.2f}%")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as summary:
            summary.write("### Coverage\n\n")
            summary.write("| metric | covered | total | percent |\n|---|---|---|---|\n")
            summary.write(
                f"| lines | {total.lines_hit} | {total.lines_found} | "
                f"{total.line_percentage:.2f}% |\n"
            )
            summary.write(
                f"| branches | {total.branches_hit} | {total.branches_found} | "
                f"{total.branch_percentage:.2f}% |\n"
            )
            summary.write(
                f"| functions | {total.functions_hit} | {total.functions_found} | "
                f"{total.function_percentage:.2f}% |\n"
            )

    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1

    print("\ncoverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
