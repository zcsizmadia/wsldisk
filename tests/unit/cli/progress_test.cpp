#include "progress.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>

#include "ops/operation.h"

using wsldisk::DiskProgress;
using wsldisk::cli::ConsoleSink;
using wsldisk::ops::StepPlan;

TEST_CASE("the console sink announces a step as it starts", "[cli][progress]") {
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.step_started(0, StepPlan{.description = "point Ubuntu at D:\\moved"});

    CHECK(out.str() == "  point Ubuntu at D:\\moved ...\n");
}

TEST_CASE("the console sink says nothing when a step finishes", "[cli][progress]") {
    // A "done" line per step doubles the output to say nothing new.
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.step_finished(0, StepPlan{.description = "point Ubuntu at D:\\moved"});

    CHECK(out.str().empty());
}

TEST_CASE("the console sink reports a percentage", "[cli][progress]") {
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.step_progress(DiskProgress{.current = 25, .total = 100});

    CHECK(out.str().find("25%") != std::string::npos);
}

TEST_CASE("the console sink says nothing about a total it does not know", "[cli][progress]") {
    // A percentage of nothing. The operation is running and has not said how
    // much there is to do, which is not something to print.
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.step_progress(DiskProgress{.current = 25, .total = 0});

    CHECK(out.str().empty());
}

TEST_CASE("the console sink passes a message through indented", "[cli][progress]") {
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.message("undoing: restore Ubuntu's BasePath");

    CHECK(out.str() == "  undoing: restore Ubuntu's BasePath\n");
}

TEST_CASE("the console sink puts a status on a line that redraws", "[cli][progress]") {
    // A countdown that scrolled would be ninety near-identical lines.
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.status("waiting for the disk ... 3s of 90s");

    CHECK(out.str() == "  waiting for the disk ... 3s of 90s\r");
}

TEST_CASE("the console sink blanks a status before printing over it", "[cli][progress]") {
    // A bare carriage return leaves the tail of the longer line behind, so a
    // countdown followed by "done" would read "  done ing for the disk ... 3s".
    std::ostringstream out;
    ConsoleSink sink{out};

    sink.status("waiting for the disk ... 3s of 90s");
    const auto after_status = out.str().size();
    sink.message("done");

    // Back to the start, as many spaces as were written, back again, then the line.
    const std::string blanked(std::string_view{"  waiting for the disk ... 3s of 90s"}.size(), ' ');
    CHECK(out.str().substr(after_status) == "\r" + blanked + "\r  done\n");
}

TEST_CASE("the console sink blanks a status nothing printed over", "[cli][progress]") {
    // The operation can end with the countdown still on screen, and the command
    // then prints its own summary. That summary goes to a clean line.
    std::ostringstream out;
    {
        ConsoleSink sink{out};
        sink.status("waiting for the disk ... 3s of 90s");
    }

    const std::string blanked(std::string_view{"  waiting for the disk ... 3s of 90s"}.size(), ' ');
    CHECK(out.str().ends_with("\r" + blanked + "\r"));
}

TEST_CASE("the console sink has nothing to blank when nothing is pending", "[cli][progress]") {
    std::ostringstream out;
    {
        ConsoleSink sink{out};
        sink.message("nothing transient here");
    }

    CHECK(out.str() == "  nothing transient here\n");
}
