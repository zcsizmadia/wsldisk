#include "completion_command.h"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "commands.h"
#include "errors.h"
#include "golden.h"

using wsldisk::ErrorCode;
using wsldisk::exit_code_for;
using wsldisk::exit_code_success;
using wsldisk::cli::add_all_commands;
using wsldisk::cli::CommandOptions;
using wsldisk::cli::completion_shells;
using wsldisk::cli::CompletionOptions;
using wsldisk::cli::generate_completion;
using wsldisk::cli::GlobalOptions;
using wsldisk::cli::run_completion;
using wsldisk::testing::Golden;

namespace {

/// The real command tree, built the way `run` builds it.
struct Tree {
    CLI::App app{"Compact, shrink, move, inspect and snapshot WSL2 virtual disks", "wsldisk"};
    CommandOptions options;

    Tree() { add_all_commands(app, options); }
};

/// Every subcommand name in the tree, including nested verbs.
[[nodiscard]] std::vector<std::string> every_command(const CLI::App& app) {
    std::vector<std::string> names;
    for (const CLI::App* command : app.get_subcommands({})) {
        names.push_back(command->get_name());
        for (const CLI::App* verb : command->get_subcommands({})) {
            names.push_back(verb->get_name());
        }
    }
    return names;
}

/// Every long flag in the tree, at every level.
[[nodiscard]] std::vector<std::string> every_flag(const CLI::App& app) {
    std::vector<std::string> names;
    const auto collect = [&names](const CLI::App& from) {
        for (const CLI::Option* option : from.get_options()) {
            if (option->get_positional()) {
                continue;
            }
            for (const std::string& name : option->get_lnames()) {
                names.push_back("--" + name);
            }
        }
    };
    collect(app);
    for (const CLI::App* command : app.get_subcommands({})) {
        collect(*command);
    }
    return names;
}

[[nodiscard]] std::string script_for(std::string_view shell) {
    const Tree tree;
    return generate_completion(tree.app, shell);
}

}  // namespace

TEST_CASE("every shell's script names every command in the tree", "[cli][completion]") {
    // Walked out of the tree rather than compared against a list written here:
    // a hand-written list is one more thing to forget when a command is added,
    // which is the whole failure this generator exists to prevent.
    const std::string shell = GENERATE(from_range(completion_shells()));
    const Tree tree;
    const std::string script = generate_completion(tree.app, shell);

    INFO("shell: " << shell);
    for (const std::string& name : every_command(tree.app)) {
        INFO("command: " << name);
        CHECK(script.find(name) != std::string::npos);
    }
}

TEST_CASE("every shell's script names every flag in the tree", "[cli][completion]") {
    const std::string shell = GENERATE(from_range(completion_shells()));
    const Tree tree;
    const std::string script = generate_completion(tree.app, shell);

    INFO("shell: " << shell);
    for (const std::string& flag : every_flag(tree.app)) {
        INFO("flag: " << flag);
        CHECK(script.find(flag) != std::string::npos);
    }
}

TEST_CASE("every shell's script asks for distributions at completion time", "[cli][completion]") {
    // Never baked in: the answer changes between one invocation and the next.
    const std::string shell = GENERATE(from_range(completion_shells()));

    const std::string script = script_for(shell);

    INFO("shell: " << shell);
    CHECK(script.find("wsldisk list --json") != std::string::npos);
}

TEST_CASE("a description with an apostrophe is escaped, not left to break the script", "[cli][completion]") {
    // `trim`'s description is "Tell one distribution's filesystem ...". An
    // unescaped apostrophe closes the quote and everything after it is syntax.
    const std::string script = script_for("zsh");

    CHECK(script.find(R"(distribution'\''s)") != std::string::npos);
    CHECK(script.find("distribution's filesystem") == std::string::npos);
}

TEST_CASE("commands with their own verbs complete to them", "[cli][completion]") {
    // `config <TAB>` offering nothing would be worse than no completion there.
    const std::string shell = GENERATE(from_range(completion_shells()));

    const std::string script = script_for(shell);

    INFO("shell: " << shell);
    for (const std::string verb : {"path", "get", "set", "edit"}) {
        INFO("verb: " << verb);
        CHECK(script.find(verb) != std::string::npos);
    }
}

TEST_CASE("the powershell script is what it was", "[cli][completion]") {
    Golden{"completion-powershell.ps1"}.check(script_for("powershell"));
}

TEST_CASE("the bash script is what it was", "[cli][completion]") {
    Golden{"completion-bash.sh"}.check(script_for("bash"));
}

TEST_CASE("the zsh script is what it was", "[cli][completion]") {
    Golden{"completion-zsh.zsh"}.check(script_for("zsh"));
}

TEST_CASE("completion writes the script for the shell it was given", "[cli][completion]") {
    const std::string shell = GENERATE(from_range(completion_shells()));
    std::ostringstream out;
    std::ostringstream err;

    const int code = run_completion(CompletionOptions{.shell = shell}, GlobalOptions{}, out, err);

    INFO("shell: " << shell);
    CHECK(code == exit_code_success);
    CHECK(out.str() == script_for(shell));
    CHECK(err.str().empty());
}

TEST_CASE("completion refuses a shell it does not know", "[cli][completion]") {
    std::ostringstream out;
    std::ostringstream err;

    const int code = run_completion(CompletionOptions{.shell = "fish"}, GlobalOptions{}, out, err);

    CHECK(code == exit_code_for(ErrorCode::Usage));
    CHECK(out.str().empty());
    CHECK(err.str().find("fish") != std::string::npos);
    // Named, so the user does not have to go and look them up.
    for (const std::string& shell : completion_shells()) {
        CHECK(err.str().find(shell) != std::string::npos);
    }
}

TEST_CASE("completion needs no registry, filesystem or WSL", "[cli][completion]") {
    // It has to work on a machine where none of those answer -- which is often
    // exactly when someone is setting their shell up.
    std::ostringstream out;
    std::ostringstream err;

    CHECK(run_completion(CompletionOptions{.shell = "bash"}, GlobalOptions{}, out, err) == exit_code_success);
    CHECK_FALSE(out.str().empty());
}

TEST_CASE("the completion verb parses its shell argument", "[cli][completion]") {
    GlobalOptions global;
    CompletionOptions options;
    CLI::App app{"wsldisk", "wsldisk"};
    wsldisk::cli::add_completion_command(app, global, options);

    std::vector<std::string> arguments{"bash", "completion"};
    app.parse(std::move(arguments));

    CHECK(options.shell == "bash");
}

TEST_CASE("completion requires a shell", "[cli][completion]") {
    GlobalOptions global;
    CompletionOptions options;
    CLI::App app{"wsldisk", "wsldisk"};
    wsldisk::cli::add_completion_command(app, global, options);

    std::vector<std::string> arguments{"completion"};
    CHECK_THROWS_AS(app.parse(std::move(arguments)), CLI::RequiredError);
}
