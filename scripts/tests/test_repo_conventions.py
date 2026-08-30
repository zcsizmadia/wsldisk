"""Conventions that break the build in ways the compiler cannot see.

These run in the `lint` job, which is on every pull request, because that is
where a check like this is actually enforced. `.githooks/pre-commit` carries the
same test-name rule, but a hook only runs when the repository's hooks are the
ones git uses -- a global `core.hooksPath`, which several tools set, silently
replaces them. A rule that depends on local configuration is not a rule.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests"

# CTest passes a test's name to Catch2 as a filter. A name starting with `--` is
# parsed as one of Catch2's own options instead, and the test fails with a
# message about an unknown flag rather than anything to do with the code.
DASH_PREFIXED_TEST = re.compile(r'TEST_CASE\(\s*"--')


def cpp_test_sources() -> list[Path]:
    return sorted(TESTS_DIR.rglob("*.cpp"))


def test_there_are_test_sources_to_check():
    # A guard on the guard: a glob that silently matches nothing would make
    # every check below pass for the wrong reason.
    assert cpp_test_sources(), f"no test sources found under {TESTS_DIR}"


def test_no_test_case_name_starts_with_a_dash():
    offenders = []
    for source in cpp_test_sources():
        text = source.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            if DASH_PREFIXED_TEST.search(line):
                offenders.append(f"{source.relative_to(REPO_ROOT)}:{number}: {line.strip()}")

    assert not offenders, (
        "CTest cannot run a test whose name starts with `--`: it hands the name "
        "to Catch2 as a filter and Catch2 parses it as one of its own options.\n"
        + "\n".join(offenders)
    )
