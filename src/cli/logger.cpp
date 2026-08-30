#include "logger.h"

#include <ostream>

namespace wsldisk::cli {

void NullLogger::warn(std::string_view message) {
    *errors_ << "warning: " << message << '\n';
}

StreamLogger::StreamLogger(std::ostream& stream, bool verbose_enabled, const std::filesystem::path& log_file)
    : stream_(&stream), verbose_enabled_(verbose_enabled) {
    if (log_file.empty()) {
        return;
    }
    // Append: a log that truncates on every run is no use for the thing logs
    // are for, which is reconstructing what happened over several attempts.
    file_.open(log_file, std::ios::app);
    if (!file_.is_open()) {
        *stream_ << "warning: could not open the log file " << log_file.string()
                 << "; continuing without it\n";
    }
}

void StreamLogger::verbose(std::string_view message) {
    // The file gets verbose detail whether or not -v was passed: someone who
    // asked for a log asked for the detail, and the flag is about the console.
    if (file_.is_open()) {
        file_ << "verbose: " << message << '\n';
    }
    if (verbose_enabled_) {
        *stream_ << message << '\n';
    }
}

void StreamLogger::warn(std::string_view message) {
    write("warning", message);
}

void StreamLogger::write(std::string_view level, std::string_view message) {
    *stream_ << level << ": " << message << '\n';
    if (file_.is_open()) {
        file_ << level << ": " << message << '\n';
    }
}

}  // namespace wsldisk::cli
