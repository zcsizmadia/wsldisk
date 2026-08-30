#!/usr/bin/env python3
"""Enforce the coverage thresholds from docs/TESTING.md against an lcov report.

Reads the lcov file produced by `llvm-cov export -format=lcov`, prints a
per-file summary, and exits non-zero if any of the line / branch / function
percentages fall below the requested threshold.

Uncovered lines are also emitted as GitHub Actions workflow annotations when
running under CI, so a failing gate points at the exact source lines in the PR
diff rather than at a percentage.

Exclusions
----------
`llvm-cov` has no notion of the `LCOV_EXCL_*` comments docs/TESTING.md allows for
provably unreachable code, so this script applies them itself by reading the
sources the report names:

    return "generic";  // LCOV_EXCL_LINE

    // LCOV_EXCL_START
    ... block excluded ...
    // LCOV_EXCL_STOP

    if (never_both_ways) {  // LCOV_EXCL_BR_LINE   (branches only, line still counts)

Every exclusion is printed on each run, and the total is capped so they cannot
accumulate unnoticed.

Defaulted special members
-------------------------
A function declared `= default` is skipped for both line and function coverage.
A compiler-generated body has no behaviour a test could assert, so requiring one
to be "covered" only buys tests that exist to call a destructor and assert
nothing. The count is printed on each run so the rule stays visible, but it is
not an exclusion and does not count against `--max-exclusions`.

Usage:
    python scripts/check-coverage.py build/x64-coverage/coverage.lcov \\
        --lines 100 --branches 100 --functions 100
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

EXCL_LINE = "LCOV_EXCL_LINE"
EXCL_START = "LCOV_EXCL_START"
EXCL_STOP = "LCOV_EXCL_STOP"
EXCL_BR_LINE = "LCOV_EXCL_BR_LINE"

# `= default;` on a special member. A compiler-generated body has no behaviour a
# test could assert, so counting one only buys a test that exists to call a
# destructor. See "Defaulted special members" in the module docstring.
DEFAULTED = re.compile(r"=\s*default\s*;")


@dataclass
class Exclusion:
    """One excluded source line, with the comment that justifies it."""

    path: str
    line: int
    kind: str
    text: str


@dataclass
class SourceExclusions:
    """Line numbers excluded in one source file."""

    lines: set[int] = field(default_factory=set)
    branch_lines: set[int] = field(default_factory=set)
    records: list[Exclusion] = field(default_factory=list)
    defaulted: set[int] = field(default_factory=set)


def read_exclusions(path: str) -> SourceExclusions:
    """Scans a source file for LCOV_EXCL markers.

    A file the report names but that is not on disk (a generated source cleaned
    up after the build, say) simply has no exclusions.
    """
    result = SourceExclusions()
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return result

    in_block = False
    block_start = 0
    for number, raw in enumerate(text.splitlines(), start=1):
        if EXCL_START in raw:
            in_block = True
            block_start = number
            result.records.append(
                Exclusion(path, number, EXCL_START, raw.strip())
            )
            continue
        if EXCL_STOP in raw:
            in_block = False
            continue
        if in_block:
            result.lines.add(number)
            result.branch_lines.add(number)
            continue
        if EXCL_BR_LINE in raw:
            result.branch_lines.add(number)
            result.records.append(Exclusion(path, number, EXCL_BR_LINE, raw.strip()))
        elif EXCL_LINE in raw:
            result.lines.add(number)
            result.branch_lines.add(number)
            result.records.append(Exclusion(path, number, EXCL_LINE, raw.strip()))
        elif DEFAULTED.search(raw):
            # Deliberately not an Exclusion record: this is a property of the
            # code rather than a judgement call someone has to defend, so it does
            # not count against --max-exclusions.
            result.defaulted.add(number)

    if in_block:
        # An unterminated block would silently swallow the rest of the file.
        raise ValueError(f"{path}:{block_start}: {EXCL_START} without a matching {EXCL_STOP}")

    return result


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
    uncovered_branches: list[int] = field(default_factory=list)
    uncovered_functions: list[tuple[int, str]] = field(default_factory=list)
    defaulted_functions: int = 0

    @staticmethod
    def percentage(hit: int, found: int) -> float:
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


def parse_lcov(
    text: str, exclusions_for=read_exclusions
) -> tuple[list[FileCoverage], list[Exclusion]]:
    """Parses the subset of lcov that llvm-cov emits, applying LCOV_EXCL markers."""
    files: list[FileCoverage] = []
    applied: list[Exclusion] = []
    current: FileCoverage | None = None
    excluded = SourceExclusions()
    # FN: announces a function and its line; FNDA: brings the hit count later.
    function_lines: dict[str, int] = {}
    function_hits: dict[str, int] = {}

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("SF:"):
            current = FileCoverage(path=line[3:])
            excluded = exclusions_for(current.path)
            applied.extend(excluded.records)
            function_lines = {}
            function_hits = {}
        elif current is None:
            continue
        elif line.startswith("DA:"):
            number_text, _, count = line[3:].partition(",")
            number = int(number_text)
            if number in excluded.lines or number in excluded.defaulted:
                continue
            current.lines_found += 1
            if int(count) > 0:
                current.lines_hit += 1
            else:
                current.uncovered_lines.append(number)
        elif line.startswith("FNDA:"):
            count, _, name = line[5:].partition(",")
            function_hits[name] = int(count)
        elif line.startswith("FN:"):
            number_text, _, name = line[3:].partition(",")
            function_lines[name] = int(number_text)
            function_hits.setdefault(name, 0)
        elif line.startswith("BRDA:"):
            parts = line[5:].split(",")
            number = int(parts[0])
            if number in excluded.branch_lines:
                continue
            taken = parts[-1]
            current.branches_found += 1
            if taken != "-" and int(taken) > 0:
                current.branches_hit += 1
            else:
                current.uncovered_branches.append(number)
        elif line == "end_of_record":
            for name, number in function_lines.items():
                if number in excluded.lines:
                    continue
                if number in excluded.defaulted:
                    current.defaulted_functions += 1
                    continue
                current.functions_found += 1
                if function_hits.get(name, 0) > 0:
                    current.functions_hit += 1
                else:
                    current.uncovered_functions.append((number, name))
            current.uncovered_functions.sort()
            files.append(current)
            current = None

    return files, applied


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
        total.defaulted_functions += file.defaulted_functions
    return total


def demangle(name: str) -> str:
    """Trims an MSVC mangled name down to something readable in a log."""
    match = re.match(r"^\?\??(?:1)?([A-Za-z_][A-Za-z0-9_]*)@([A-Za-z_][A-Za-z0-9_]*)@", name)
    if match:
        return f"{match.group(2)}::{match.group(1)}"
    return name


def report(files: list[FileCoverage], max_annotations: int) -> None:
    name_width = max((len(Path(f.path).as_posix()) for f in files), default=20)
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
        gaps: list[tuple[int, str]] = []
        gaps += [(line, "line not covered by any test") for line in file.uncovered_lines]
        gaps += [
            (line, "branch never taken in both directions") for line in file.uncovered_branches
        ]
        gaps += [
            (line, f"function never called by a test: {demangle(name)}")
            for line, name in file.uncovered_functions
        ]
        for line, message in sorted(gaps):
            if annotations >= max_annotations:
                break
            annotate(display, line, message)
            annotations += 1

    total = totals(files)
    print("-" * len(header))
    print(
        f"{'TOTAL':<{name_width}}  "
        f"{total.line_percentage:7.2f}%  "
        f"{total.branch_percentage:8.2f}%  "
        f"{total.function_percentage:9.2f}%"
    )
    if total.defaulted_functions:
        plural = "s" if total.defaulted_functions != 1 else ""
        print(f"\n{total.defaulted_functions} defaulted special member{plural} not counted")


def write_step_summary(total: FileCoverage) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    with open(summary_path, "a", encoding="utf-8") as summary:
        summary.write("### Coverage\n\n")
        summary.write("| metric | covered | total | percent |\n|---|---|---|---|\n")
        for label, hit, found, percent in (
            ("lines", total.lines_hit, total.lines_found, total.line_percentage),
            ("branches", total.branches_hit, total.branches_found, total.branch_percentage),
            ("functions", total.functions_hit, total.functions_found, total.function_percentage),
        ):
            summary.write(f"| {label} | {hit} | {found} | {percent:.2f}% |\n")


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
        help="stop annotating coverage gaps after this many",
    )
    parser.add_argument(
        "--max-exclusions",
        type=int,
        default=10,
        help="fail if more than this many LCOV_EXCL markers are in use",
    )
    args = parser.parse_args(argv)

    if not args.lcov.is_file():
        print(f"error: no lcov report at {args.lcov}", file=sys.stderr)
        return 2

    try:
        files, exclusions = parse_lcov(args.lcov.read_text(encoding="utf-8"))
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if not files:
        print(f"error: {args.lcov} contains no records", file=sys.stderr)
        return 2

    report(files, args.max_annotations)

    if exclusions:
        print(f"\ncoverage exclusions in use ({len(exclusions)}):")
        for exclusion in exclusions:
            print(f"  {Path(exclusion.path).as_posix()}:{exclusion.line}: {exclusion.text}")

    total = totals(files)
    write_step_summary(total)

    failures = []
    for label, actual, required in (
        ("line", total.line_percentage, args.lines),
        ("branch", total.branch_percentage, args.branches),
        ("function", total.function_percentage, args.functions),
    ):
        if actual + 1e-9 < required:
            failures.append(f"{label} coverage {actual:.2f}% is below the required {required:.2f}%")

    if len(exclusions) > args.max_exclusions:
        failures.append(
            f"{len(exclusions)} coverage exclusions are in use, more than the "
            f"{args.max_exclusions} docs/TESTING.md allows"
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
