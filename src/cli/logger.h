#pragma once

#include <fstream>
#include <iosfwd>
#include <string_view>

#include "options.h"

namespace wsldisk::cli {

/// Where verbose output goes.
///
/// An interface so a test can read what was logged, and so the destination is
/// a decision made once at startup rather than at every call site.
class ILogger {
public:
    ILogger() = default;
    ILogger(const ILogger&) = delete;
    ILogger& operator=(const ILogger&) = delete;
    ILogger(ILogger&&) = delete;
    ILogger& operator=(ILogger&&) = delete;
    virtual ~ILogger() = default;

    /// Detail the user asked for with `-v`. Never appears on stdout.
    virtual void verbose(std::string_view message) = 0;

    /// Something the user should see whether or not they asked for detail.
    virtual void warn(std::string_view message) = 0;
};

/// Discards everything. The default when neither `-v` nor `--log` was given.
class NullLogger final : public ILogger {
public:
    void verbose(std::string_view /*message*/) override {}

    void warn(std::string_view message) override;

    explicit NullLogger(std::ostream& errors) : errors_(&errors) {}

private:
    // Warnings still have somewhere to go: they are not verbose detail, and
    // silencing them would hide a skipped registry key.
    std::ostream* errors_;
};

/// Writes to a stream, and optionally appends to a file as well.
///
/// The stream is stderr in production. That is not a detail: `--json` puts
/// machine-readable output on stdout, and a stray verbose line there would make
/// it unparseable.
class StreamLogger final : public ILogger {
public:
    /// `log_file` empty means stream only. A file that cannot be opened is
    /// reported once on the stream and then ignored -- failing the command
    /// because the log could not be written would be worse than not logging.
    StreamLogger(std::ostream& stream, bool verbose_enabled, const std::filesystem::path& log_file);

    void verbose(std::string_view message) override;
    void warn(std::string_view message) override;

    /// Whether the log file was asked for and could be opened.
    [[nodiscard]] bool logging_to_file() const noexcept { return file_.is_open(); }

private:
    void write(std::string_view level, std::string_view message);

    std::ostream* stream_;
    bool verbose_enabled_;
    std::ofstream file_;
};

}  // namespace wsldisk::cli
