"""Conventions that break the build in ways the compiler cannot see.

These run in the `lint` job, which is on every pull request, because that is
where a check like this is actually enforced. `.githooks/pre-commit` carries the
same test-name rule, but a hook only runs when the repository's hooks are the
ones git uses -- a global `core.hooksPath`, which several tools set, silently
replaces them. A rule that depends on local configuration is not a rule.
"""

from __future__ import annotations

import json
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
RAW_STRING_OPEN = re.compile(r'R"([^("\\s]*)\(')


def string_literals(source: str) -> list[str]:
    """Every string literal in a C++ source, skipping comments.

    A regex is not enough: the first version of this check flagged
    `app_test.cpp` for the word `compact` inside a comment that quotes the very
    bug the check exists to prevent. A guard that cannot tell code from prose
    about code is a guard people learn to work around.
    """
    literals: list[str] = []
    index = 0
    end = len(source)
    while index < end:
        two = source[index : index + 2]
        if two == "//":
            index = source.find("\n", index)
            if index == -1:
                break
            continue
        if two == "/*":
            closed = source.find("*/", index + 2)
            index = end if closed == -1 else closed + 2
            continue
        if source[index] == "'":
            # A character literal, including '\'' -- never a command name.
            index += 3 if source[index : index + 2] == "'\\" else 2
            index = source.find("'", index) + 1 if index <= end else end
            continue
        raw = RAW_STRING_OPEN.match(source, index)
        if raw:
            terminator = ')' + raw.group(1) + '"'
            closed = source.find(terminator, raw.end())
            if closed == -1:
                break
            literals.append(source[raw.end() : closed])
            index = closed + len(terminator)
            continue
        if source[index] == '"':
            index += 1
            start = index
            while index < end and source[index] != '"':
                index += 2 if source[index] == "\\" else 1
            literals.append(source[start:index])
            index += 1
            continue
        index += 1
    return literals


# `cli::run` and `main_entry` construct the *real* Win32Registry, Win32FileSystem
# and WslExeHost. Anything a unit test hands them acts on the machine running the
# suite.
DRIVES_THE_REAL_SERVICES = re.compile(r"\b(?:cli::run|main_entry)\s*\(")

# Where the command tree declares its subcommands. Read rather than listed, so a
# new command is covered the day it lands.
ADD_SUBCOMMAND = re.compile(r'add_subcommand\(\s*"([a-z][a-z0-9-]*)"')

CLI_SOURCES = REPO_ROOT / "src" / "cli"


def subcommand_names() -> set[str]:
    names: set[str] = set()
    for source in sorted(CLI_SOURCES.glob("*.cpp")):
        names.update(ADD_SUBCOMMAND.findall(source.read_text(encoding="utf-8", errors="replace")))
    return names


def unit_test_sources() -> list[Path]:
    return sorted((TESTS_DIR / "unit").rglob("*.cpp"))


def test_the_command_tree_declares_subcommands():
    # A guard on the guard: an empty set would make the check below pass for
    # every file, which is exactly the failure it exists to prevent.
    names = subcommand_names()
    assert "compact" in names, f"no subcommands found in {CLI_SOURCES}; the check below is inert"


def test_no_unit_test_names_a_subcommand_it_could_run():
    """A unit test that can reach `cli::run` must not be able to name a command.

    The original form of this check looked at one call pattern in one file, and
    `plumbing_test.cpp` was already calling `cli::run` directly where it could
    not see. It also could not see through a named argument vector, which is how
    those calls are written.

    So the rule is blunter and does not depend on how the call is spelled: in a
    unit-test file that reaches the real services, the *name of a subcommand may
    not appear at all*. You cannot invoke what you cannot name. Drive a command
    through its own `run_*` entry point with fakes instead -- which is what every
    `*_command_test.cpp` already does.

    This is deliberately stricter than the danger: `completion` touches nothing.
    A guard that has to reason about which commands are safe is a guard that
    will one day reason wrongly, and this one is here because it already
    happened -- `app_test.cpp` asserted `{"compact", "Ubuntu"}` was an unknown
    subcommand, and when `compact` shipped, the unit suite ran `fstrim` inside a
    real Ubuntu and terminated it.
    """
    commands = subcommand_names()
    offenders = []
    for source in unit_test_sources():
        text = source.read_text(encoding="utf-8", errors="replace")
        if not DRIVES_THE_REAL_SERVICES.search(text):
            continue
        named = sorted({literal for literal in string_literals(text) if literal in commands})
        if named:
            offenders.append(f"{source.relative_to(REPO_ROOT)}: {', '.join(named)}")

    assert not offenders, (
        "these unit tests reach cli::run/main_entry, which wires the real "
        "registry, filesystem and wsl.exe, and they name a real subcommand -- so "
        "they can act on the machine running the suite. Use a `wsldisk-` "
        "prefixed placeholder, or drive the command through its own run_* entry "
        "point with fakes.\n" + "\n".join(offenders)
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


PACKAGING_DIR = REPO_ROOT / "packaging"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"

# `__VERSION__`, `__SHA256_X64__` -- the holes release.yml fills in before the
# manifests are submitted to a package repository.
PLACEHOLDER = re.compile(r"__[A-Z0-9_]+__")


def winget_manifests() -> list[Path]:
    return sorted((PACKAGING_DIR / "winget").glob("*.yaml"))


def test_there_are_winget_manifests_to_check():
    assert winget_manifests(), "no winget manifests found"


def test_every_manifest_placeholder_is_substituted_by_the_release_workflow():
    # The workflow refuses to publish a manifest with a surviving placeholder,
    # so getting this wrong does not ship a broken package -- it fails the
    # release, after the tag is pushed and the artifacts are signed, which is a
    # miserable place to find out. Adding a placeholder to a manifest without
    # teaching release.yml to fill it should fail here instead.
    workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")
    substituted = set(PLACEHOLDER.findall(workflow))

    offenders = []
    for manifest in winget_manifests():
        for name in set(PLACEHOLDER.findall(manifest.read_text(encoding="utf-8"))):
            if name not in substituted:
                offenders.append(
                    f"{manifest.relative_to(REPO_ROOT)}: {name} is never substituted"
                )

    assert not offenders, (
        "release.yml has no sed expression for these placeholders.\n"
        + "\n".join(offenders)
    )


def test_the_scoop_manifest_is_parseable_json_with_a_placeholder_version():
    # release.yml rewrites this file with `json.load`, so a syntax error here
    # only surfaces during a release. The placeholder version matters too: the
    # substitution finds the URLs by replacing the literal `0.0.0`.
    manifest = json.loads(
        (PACKAGING_DIR / "scoop" / "wsldisk.json").read_text(encoding="utf-8")
    )
    assert manifest["version"] == "0.0.0", (
        "the checked-in scoop manifest should carry the placeholder version; "
        "release.yml substitutes the real one"
    )
    for architecture in ("64bit", "arm64"):
        entry = manifest["architecture"][architecture]
        assert "0.0.0" in entry["url"], f"{architecture} url has no version to substitute"
        assert set(entry["hash"]) == {"0"}, f"{architecture} hash is not a placeholder"


def test_string_literals_skips_comments():
    # The case that broke the first version of the subcommand check: a comment
    # quoting the bad code it warns about.
    source = '// still said `{"compact", "Ubuntu"}`.\nconst char* ok = "safe";\n'
    assert string_literals(source) == ["safe"]


def test_string_literals_skips_block_comments():
    assert string_literals('/* "compact" */ auto x = "kept";') == ["kept"]


def test_string_literals_reads_raw_strings():
    # Test sources are full of these for Windows paths.
    assert string_literals(r'const auto p = R"(D:\moved\ext4.vhdx)";') == [r"D:\moved\ext4.vhdx"]


def test_string_literals_handles_escaped_quotes_and_char_literals():
    # An escaped quote must not end the literal early, and `'"'` must not start
    # one -- either mistake would desynchronise the rest of the file.
    source = 'auto a = "he said \\"no\\""; char q = \'"\'; auto b = "after";'
    assert string_literals(source) == ['he said \\"no\\"', "after"]


def test_string_literals_does_not_mistake_a_url_for_a_comment():
    assert string_literals('auto u = "https://example.com/x"; auto v = "next";') == [
        "https://example.com/x",
        "next",
    ]


FUZZ_CMAKE = TESTS_DIR / "fuzz" / "CMakeLists.txt"
NIGHTLY_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "nightly.yml"

ADD_FUZZER = re.compile(r"^wsldisk_add_fuzzer\((\w+)", re.MULTILINE)


def fuzz_targets() -> set[str]:
    return set(ADD_FUZZER.findall(FUZZ_CMAKE.read_text(encoding="utf-8")))


def test_there_are_fuzz_targets_to_check():
    assert fuzz_targets(), f"no wsldisk_add_fuzzer calls found in {FUZZ_CMAKE}"


def test_every_fuzz_target_is_fuzzed_nightly():
    """A target absent from the nightly matrix is never actually fuzzed.

    It still replays its seed corpus at `-runs=0` on every pull request, which
    looks like coverage on the check list and is not: nothing new is ever tried.
    `fuzz_parse_distro` and `fuzz_parse_config` sat in that state from the day
    they were added -- the config parser being the one the docs single out as
    most worth fuzzing.
    """
    workflow = NIGHTLY_WORKFLOW.read_text(encoding="utf-8")
    missing = sorted(target for target in fuzz_targets() if target not in workflow)

    assert not missing, (
        "these fuzz targets exist but are not in nightly.yml's matrix, so they "
        "are only ever replayed, never fuzzed: " + ", ".join(missing)
    )
