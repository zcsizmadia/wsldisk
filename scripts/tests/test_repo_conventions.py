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


# `cli::run()` wires up the real Win32 services, so a unit test that hands it a
# subcommand acts on the machine the suite runs on. That is not theoretical:
# `app_test.cpp` once asserted that `{"compact", "Ubuntu"}` was an unknown
# subcommand, and when `compact` landed the case started running fstrim inside
# the developer's real Ubuntu and terminating it.
#
# Every positional argument in that file must therefore be a name nothing will
# ever register. `wsldisk-` prefixed placeholders are, and so is anything that
# is a flag rather than a positional.
INVOKE_ARGUMENTS = re.compile(r"invoke\(\{([^}]*)\}\)")
STRING_LITERAL = re.compile(r'"([^"]*)"')


def test_app_test_never_hands_run_a_real_subcommand():
    source = TESTS_DIR / "unit" / "cli" / "app_test.cpp"
    assert source.is_file(), f"{source} has moved; update this convention check"

    text = source.read_text(encoding="utf-8", errors="replace")
    offenders = []
    for call in INVOKE_ARGUMENTS.finditer(text):
        for argument in STRING_LITERAL.findall(call.group(1)):
            if argument.startswith("-") or argument.startswith("wsldisk-") or not argument:
                continue
            offenders.append(argument)

    assert not offenders, (
        "cli::run() constructs the real registry, filesystem and wsl.exe wrapper, "
        "so a unit test must never hand it a name that is (or becomes) a "
        "subcommand -- it would act on the machine running the suite. Use a "
        "`wsldisk-` prefixed placeholder instead of: " + ", ".join(sorted(set(offenders)))
    )


# markdownlint's MD051 catches a link to a heading anchor that does not exist,
# and it is the one lint that cannot run on a developer machine here: the npm
# proxy returns 403 Forbidden for markdownlint-cli2, so `scripts/lint.ps1`
# always reports it SKIPPED and CI is the first place the failure appears.
#
# This reimplements just that rule, in the pytest step that does run locally. It
# caught nothing that markdownlint would not, but it catches it a CI round trip
# earlier -- which is the whole point.
MARKDOWN_FILES = ("README.md", "CONTRIBUTING.md", "PLAN.md", "ROADMAP.md")
SAME_FILE_LINK = re.compile(r"\]\(#([^)]+)\)")
ATX_HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
FENCE = re.compile(r"^\s*```")


def markdown_documents() -> list[Path]:
    documents = [REPO_ROOT / name for name in MARKDOWN_FILES]
    documents.extend(sorted((REPO_ROOT / "docs").glob("*.md")))
    return [document for document in documents if document.is_file()]


def heading_anchors(text: str) -> set[str]:
    """The anchors GitHub generates for a document's headings.

    Lowercase, drop everything that is not alphanumeric, space or hyphen, then
    turn spaces into hyphens. Backticks and quotes vanish; the hyphens inside
    `--terminate` survive, which is exactly how a heading can end up with three
    of them in a row and a hand-written fragment can be wrong.
    """
    anchors: set[str] = set()
    seen: dict[str, int] = {}
    in_fence = False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = ATX_HEADING.match(line)
        if not match:
            continue
        slug = "".join(
            character
            for character in match.group(2).lower()
            if character.isalnum() or character in " -_"
        ).strip().replace(" ", "-")
        # A repeated heading gets `-1`, `-2`, ... appended, same as GitHub.
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        anchors.add(slug if count == 0 else f"{slug}-{count}")
    return anchors


def test_there_are_markdown_documents_to_check():
    # A guard on the guard: an empty list would make the check below pass for
    # the wrong reason.
    assert markdown_documents(), "no markdown documents found"


def test_every_same_document_link_fragment_resolves():
    offenders = []
    for document in markdown_documents():
        text = document.read_text(encoding="utf-8", errors="replace")
        anchors = heading_anchors(text)
        for fragment in SAME_FILE_LINK.findall(text):
            if fragment not in anchors:
                offenders.append(
                    f"{document.relative_to(REPO_ROOT)}: #{fragment} matches no heading"
                )

    assert not offenders, (
        "markdownlint MD051 will fail on these, and it only runs in CI here -- "
        "the npm proxy forbids markdownlint-cli2 locally.\n" + "\n".join(offenders)
    )
