#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "errors.h"
#include "model/disk_info.h"
#include "model/distro.h"

namespace wsldisk::cli {

/// What every command prints as one line of a table or one JSON object.
///
/// A cell that could not be measured is `std::nullopt` rather than zero or an
/// empty string: "we do not know" and "it is nothing" are different answers and
/// the output has to keep them apart.
struct Cell {
    /// Already-formatted text, for names and paths.
    std::optional<std::string> text;
    /// A byte count, rendered by `format_size` for the table and as an integer
    /// for JSON. Kept as a number so the two renderers cannot disagree.
    std::optional<std::uint64_t> bytes;
    std::optional<bool> flag;
};

/// A table that sizes its own columns.
class Table {
public:
    explicit Table(std::vector<std::string> headers) : headers_(std::move(headers)) {}

    /// Adds a row. A row with the wrong number of cells is a programming error
    /// and is padded rather than throwing, because a half-drawn table is more
    /// useful than a crash while reporting something else.
    void add_row(std::vector<Cell> cells);

    /// Renders to `out`. Columns are as wide as their widest cell, and a cell
    /// with no value prints `-`.
    void render(std::ostream& out) const;

private:
    std::vector<std::string> headers_;
    std::vector<std::vector<Cell>> rows_;
};

/// Aligned `key: value` lines.
///
/// What `info` and `config` both print: one subject and a list of fields, which
/// is a list rather than a grid. Shared so the two cannot end up aligning
/// differently.
class Details {
public:
    void add(std::string key, std::string value);

    /// A value that could not be measured still gets a line: a missing row is
    /// invisible, and the point of these commands is to say what is and is not
    /// known.
    void add(std::string key, const std::optional<std::uint64_t>& bytes);

    void write(std::ostream& out) const;

private:
    std::vector<std::pair<std::string, std::string>> lines_;
    std::size_t width_ = 0;
};

/// One distribution as a JSON object, on a single line.
///
/// Sizes are integers in bytes, never formatted strings: a consumer that wants
/// "14.2 GiB" can format it, but one that wants to compare or sum cannot
/// un-format it. The schema is documented in docs/JSON.md.
[[nodiscard]] std::string to_json_line(const model::Distro& distro, const model::DiskInfo& info);

/// An error as a JSON object, using the same stable token as `--json` output
/// elsewhere so a script can branch on it.
[[nodiscard]] std::string to_json_line(const Error& error);

/// `message -- remedy`, which is what the top-level handler prints.
[[nodiscard]] std::string to_human_line(const Error& error);

}  // namespace wsldisk::cli
