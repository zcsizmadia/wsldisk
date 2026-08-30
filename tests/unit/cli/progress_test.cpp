#include "progress.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

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
