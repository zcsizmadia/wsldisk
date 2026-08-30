#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace wsldisk::model {

/// Decodes UTF-16LE bytes into UTF-8.
///
/// `wsl.exe` writes its own output as UTF-16LE, sometimes with a byte-order
/// mark and sometimes without (measured, docs/RESEARCH.md). Nothing else about
/// the stream is guaranteed: a pipe read can end mid-character, leaving an odd
/// byte count or a high surrogate with no low one. Both are tolerated -- a
/// trailing half-unit is dropped and an unpaired surrogate becomes U+FFFD --
/// because the alternative is a decoder that throws on a truncated read.
///
/// Embedded NULs are decoded as U+0000 rather than discarded. Treating them as
/// end-of-string is what makes callers see a distribution list as one name.
[[nodiscard]] std::string decode_utf16le(std::span<const std::byte> bytes);

/// Distribution names from `wsl --list --quiet` output.
///
/// `--quiet` prints one name per line and nothing else, so this never parses
/// prose. Lines are CRLF-terminated; blank ones are dropped, which covers the
/// trailing newline and the stray blank line WSL emits on some builds.
[[nodiscard]] std::vector<std::string> parse_distribution_list(std::span<const std::byte> bytes);

}  // namespace wsldisk::model
