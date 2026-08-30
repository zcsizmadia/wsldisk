#include "render.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ostream>

#include "model/size.h"

namespace wsldisk::cli {
namespace {

/// What an unmeasurable cell prints as. A dash rather than a blank, so a column
/// that is entirely unknown still looks like a column.
constexpr std::string_view unknown = "-";

/// Two spaces between columns: enough to read, narrow enough that a table with
/// a long path still fits a normal terminal.
constexpr std::size_t column_gap = 2;

[[nodiscard]] std::string format_cell(const Cell& cell) {
    if (cell.text.has_value()) {
        return *cell.text;
    }
    if (cell.bytes.has_value()) {
        return format_size(*cell.bytes);
    }
    if (cell.flag.has_value()) {
        return *cell.flag ? "yes" : "no";
    }
    return std::string{unknown};
}

/// Adds a value only when it is known. An absent field is left out of the
/// object rather than written as null: a consumer checking `"guest_used" in obj`
/// gets the same answer as one checking for null, and the line stays shorter.
template <typename T>
void add_if_known(nlohmann::json& object, std::string_view key, const std::optional<T>& value) {
    if (value.has_value()) {
        object[std::string{key}] = *value;
    }
}

/// Every cell of a row as the text it will print as.
[[nodiscard]] std::vector<std::string> formatted(const std::vector<Cell>& row) {
    std::vector<std::string> text;
    text.reserve(row.size());
    for (const Cell& cell : row) {
        text.push_back(format_cell(cell));
    }
    return text;
}

}  // namespace

void Table::add_row(std::vector<Cell> cells) {
    cells.resize(headers_.size());
    rows_.push_back(std::move(cells));
}

void Table::render(std::ostream& out) const {
    std::vector<std::size_t> widths;
    widths.reserve(headers_.size());
    for (const std::string& header : headers_) {
        widths.push_back(header.size());
    }

    std::vector<std::vector<std::string>> body;
    body.reserve(rows_.size());
    for (const std::vector<Cell>& row : rows_) {
        std::vector<std::string> text = formatted(row);
        for (std::size_t column = 0; column < text.size(); ++column) {
            widths[column] = std::max(widths[column], text[column].size());
        }
        body.push_back(std::move(text));
    }

    const auto write_row = [&out, &widths](const std::vector<std::string>& cells) {
        for (std::size_t column = 0; column < cells.size(); ++column) {
            out << cells[column];
            // No padding after the last column: trailing spaces are invisible
            // until someone diffs the output, and the golden files do.
            if (column + 1 < cells.size()) {
                out << std::string(widths[column] - cells[column].size() + column_gap, ' ');
            }
        }
        out << '\n';
    };

    write_row(headers_);
    for (const std::vector<std::string>& row : body) {
        write_row(row);
    }
}

std::string to_json_line(const model::Distro& distro, const model::DiskInfo& info) {
    nlohmann::json object;
    object["name"] = distro.name;
    object["guid"] = distro.guid;
    object["version"] = distro.version;
    object["default"] = distro.is_default;
    object["vhdx_path"] = distro.vhdx_path.string();

    if (!distro.flavor.empty()) {
        object["flavor"] = distro.flavor;
    }
    if (!distro.os_version.empty()) {
        object["os_version"] = distro.os_version;
    }

    add_if_known(object, "virtual_size", info.virtual_size);
    add_if_known(object, "file_size", info.file_size);
    add_if_known(object, "size_on_disk", info.size_on_disk);
    add_if_known(object, "allocated_bytes", info.allocated_bytes);
    add_if_known(object, "sparse", info.is_sparse);
    add_if_known(object, "guest_used", info.guest_used);
    add_if_known(object, "guest_free", info.guest_free);
    add_if_known(object, "reclaimable", info.reclaimable());

    if (!info.notes.empty()) {
        object["notes"] = info.notes;
    }
    return object.dump();
}

std::string to_json_line(const Error& error) {
    nlohmann::json object;
    object["error"] = error_code_name(error.code);
    object["exit_code"] = exit_code_for(error.code);
    object["message"] = error.message;
    if (!error.remedy.empty()) {
        object["remedy"] = error.remedy;
    }
    return object.dump();
}

std::string to_human_line(const Error& error) {
    return error.to_string();
}

}  // namespace wsldisk::cli
