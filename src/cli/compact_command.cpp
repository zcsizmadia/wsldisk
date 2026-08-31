#include "compact_command.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <ostream>
#include <vector>

#include "app.h"
#include "logger.h"
#include "lookup.h"
#include "model/size.h"
#include "ops/compact.h"
#include "ops/runner.h"
#include "ops/trim.h"
#include "preflight.h"
#include "progress.h"
#include "render.h"

namespace wsldisk::cli {
namespace {

/// One thing that was compacted, or that failed to be.
struct Outcome {
    std::string label;
    std::optional<Error> failure;
    std::optional<std::uint64_t> before;
    std::optional<std::uint64_t> after;
    std::optional<std::uint64_t> reclaimed;
};

/// The flags, over the configured defaults.
///
/// Both flags are one-way, which is what lets this fold them together without
/// asking CLI11 whether they were given: `--no-trim` can only turn trimming off
/// and `--restart` can only turn restarting on. A setting the flag cannot
/// express -- `wsl.unlock_timeout_seconds` -- comes from the file alone.
[[nodiscard]] ops::CompactOptions to_operation_options(const CompactCommandOptions& options,
                                                       const model::Config& config) {
    return ops::CompactOptions{.trim = config.compact_trim && !options.no_trim,
                               .shutdown = options.shutdown,
                               .restart = config.compact_restart || options.restart,
                               .unlock_timeout = config.unlock_timeout()};
}

void render_outcome(const Outcome& outcome, std::ostream& out) {
    if (outcome.failure.has_value()) {
        out << outcome.label << ": " << to_human_line(*outcome.failure) << '\n';
        return;
    }
    if (!outcome.reclaimed.has_value()) {
        // Compacted, but the file could not be measured at one end or the other.
        // Saying so beats printing a number that was never taken.
        out << outcome.label << ": compacted. Its size could not be measured.\n";
        return;
    }
    out << outcome.label << ": " << format_size(*outcome.reclaimed) << " reclaimed ("
        << format_size(outcome.before.value_or(0)) << " to " << format_size(outcome.after.value_or(0))
        << ")\n";
}

void render_outcome_json(const Outcome& outcome, std::ostream& out) {
    nlohmann::json object;
    object["target"] = outcome.label;
    object["compacted"] = !outcome.failure.has_value();
    if (outcome.failure.has_value()) {
        object["error"] = outcome.failure->message;
        object["remedy"] = outcome.failure->remedy;
        object["exit_code"] = exit_code_for(outcome.failure->code);
    }
    if (outcome.before.has_value()) {
        object["size_before"] = *outcome.before;
    }
    if (outcome.after.has_value()) {
        object["size_after"] = *outcome.after;
    }
    if (outcome.reclaimed.has_value()) {
        object["reclaimed"] = *outcome.reclaimed;
    }
    out << object.dump() << '\n';
}

/// Runs one compaction and turns whatever happened into an `Outcome`.
///
/// A failure is carried rather than thrown so `--all` can report every disk it
/// tried: stopping at the first one that will not compact leaves the user to
/// work out how far it got.
[[nodiscard]] Outcome compact_one(ops::CompactOperation& operation, std::string label,
                                  const GlobalOptions& global, ops::ProgressSink& sink) {
    Outcome outcome{.label = std::move(label)};

    const auto result = ops::run(operation, sink, ops::RunOptions{.dry_run = global.dry_run});
    if (!result.has_value()) {
        outcome.failure = result.error();
        return outcome;
    }
    if (global.dry_run) {
        return outcome;
    }

    outcome.before = operation.size_before();
    outcome.after = operation.size_after();
    outcome.reclaimed = operation.reclaimed();
    return outcome;
}

/// The distributions `--all` should act on.
///
/// WSL1 entries are left out rather than reported as failures: they have no
/// virtual disk, so `compact --all` skipping them is right, and one refusal per
/// WSL1 distribution would be noise on every run.
[[nodiscard]] Result<std::vector<model::Distro>> compactable(const Services& services, ILogger& logger) {
    const auto distros = model::enumerate(*services.registry);
    if (!distros.has_value()) {
        return std::unexpected(distros.error());
    }
    for (const std::string& warning : distros->warnings) {
        logger.warn(warning);
    }

    std::vector<model::Distro> wsl2;
    for (const model::Distro& distro : distros->distros) {
        if (distro.is_wsl2()) {
            wsl2.push_back(distro);
        } else {
            logger.verbose(distro.name + " is WSL1 and has no virtual disk; skipped");
        }
    }
    return wsl2;
}

void render_dry_run(const ops::Plan& plan, std::ostream& out) {
    out << "--dry-run: nothing was changed. It would have:\n";
    for (const ops::StepPlan& step : plan.steps) {
        out << "  " << step.description << '\n';
    }
}

/// Compacts a `.vhdx` that no distribution claims.
///
/// Its own function because it has none of the guest steps and none of the
/// per-distribution reporting: one target, one answer, one exit code.
[[nodiscard]] int compact_loose_file(const Services& services, const CompactCommandOptions& options,
                                     const GlobalOptions& global, ops::ProgressSink& sink, std::ostream& out,
                                     std::ostream& err) {
    ops::CompactOperation operation{*services.disks,
                                    *services.filesystem,
                                    *services.host,
                                    *services.clock,
                                    std::filesystem::path{options.file},
                                    to_operation_options(options, services.config)};
    const auto planned = operation.plan();
    if (!planned.has_value()) {
        return report(planned.error(), global, out, err);
    }

    const Outcome outcome = compact_one(operation, options.file, global, sink);
    if (outcome.failure.has_value()) {
        return report(*outcome.failure, global, out, err);
    }
    if (global.dry_run) {
        render_dry_run(*planned, out);
        return exit_code_success;
    }
    if (global.json) {
        render_outcome_json(outcome, out);
    } else {
        render_outcome(outcome, out);
    }
    return exit_code_success;
}

/// Compacts each target, collecting an outcome per distribution.
///
/// A dry run renders its plan as it goes and collects only the refusals, since
/// there is no result to report for one that would have worked.
[[nodiscard]] std::vector<Outcome> compact_each(const Services& services,
                                                const std::vector<model::Distro>& targets,
                                                const CompactCommandOptions& options,
                                                const GlobalOptions& global, ops::ProgressSink& sink,
                                                std::ostream& out) {
    std::vector<Outcome> outcomes;
    for (const model::Distro& distro : targets) {
        ops::CompactOperation operation{*services.disks, *services.filesystem,
                                        *services.host,  *services.clock,
                                        distro,          to_operation_options(options, services.config)};
        if (!global.dry_run) {
            outcomes.push_back(compact_one(operation, distro.name, global, sink));
            continue;
        }

        const auto planned = operation.plan();
        if (!planned.has_value()) {
            outcomes.push_back(Outcome{.label = distro.name, .failure = planned.error()});
            continue;
        }
        out << distro.name << ":\n";
        render_dry_run(*planned, out);
    }
    return outcomes;
}

/// A dry run's exit code: anything that refused at plan time is still a refusal.
///
/// Reporting success for a run that would not have worked would be telling the
/// user something that is not going to happen.
[[nodiscard]] int report_dry_run(const std::vector<Outcome>& refusals, std::ostream& err) {
    for (const Outcome& outcome : refusals) {
        render_outcome(outcome, err);
    }
    return refusals.empty() ? exit_code_success : exit_code_for(ErrorCode::Preflight);
}

/// Prints every outcome and returns the exit code for the set.
[[nodiscard]] int report_outcomes(const std::vector<Outcome>& outcomes, const GlobalOptions& global,
                                  std::ostream& out, std::ostream& err) {
    std::uint64_t total = 0;
    for (const Outcome& outcome : outcomes) {
        if (global.json) {
            render_outcome_json(outcome, out);
        } else if (outcome.failure.has_value()) {
            render_outcome(outcome, err);
        } else {
            render_outcome(outcome, out);
            total += outcome.reclaimed.value_or(0);
        }
    }

    for (const Outcome& outcome : outcomes) {
        // The first failure's code, so a single-target run still exits with the
        // specific reason -- 11 for a busy disk, 3 for a preflight refusal.
        if (outcome.failure.has_value()) {
            return exit_code_for(outcome.failure->code);
        }
    }

    if (!global.json && outcomes.size() > 1) {
        out << '\n' << format_size(total) << " reclaimed in total\n";
    }
    return exit_code_success;
}

/// The distributions the command was pointed at: one by name, or all of them.
[[nodiscard]] Result<std::vector<model::Distro>> targets_of(const Services& services,
                                                            const CompactCommandOptions& options,
                                                            ILogger& logger) {
    if (options.all) {
        return compactable(services, logger);
    }

    const auto one = find_distro(*services.registry, options.name, logger);
    if (!one.has_value()) {
        return std::unexpected(one.error());
    }
    if (const Status supported = require_wsl2(*one); !supported.has_value()) {
        return std::unexpected(supported.error());
    }
    return std::vector<model::Distro>{*one};
}

}  // namespace

bool CompactCommandOptions::targets_one_thing() const noexcept {
    const int named = (name.empty() ? 0 : 1) + (all ? 1 : 0) + (file.empty() ? 0 : 1);
    return named == 1;
}

int run_compact(const Services& services, const CompactCommandOptions& options, const GlobalOptions& global,
                ILogger& logger, std::ostream& out, std::ostream& err) {
    if (!options.targets_one_thing()) {
        return report(Error{ErrorCode::Usage, "name one distribution, --all, or --file",
                            "`wsldisk compact Ubuntu`, `wsldisk compact --all`, or "
                            "`wsldisk compact --file D:\\disks\\data.vhdx`"},
                      global, out, err);
    }

    ConsoleSink progress{out};
    ops::NullSink quiet;
    ops::ProgressSink& sink = global.json ? static_cast<ops::ProgressSink&>(quiet) : progress;

    // A loose file: no guest to trim, nothing to terminate.
    if (!options.file.empty()) {
        return compact_loose_file(services, options, global, sink, out, err);
    }

    const auto chosen = targets_of(services, options, logger);
    if (!chosen.has_value()) {
        return report(chosen.error(), global, out, err);
    }
    const std::vector<model::Distro>& targets = *chosen;

    if (targets.empty()) {
        out << "no WSL2 distributions to compact\n";
        return exit_code_success;
    }

    const std::vector<Outcome> outcomes = compact_each(services, targets, options, global, sink, out);
    if (global.dry_run) {
        return report_dry_run(outcomes, err);
    }
    return report_outcomes(outcomes, global, out, err);
}

void add_compact_command(CLI::App& app, GlobalOptions& global, CompactCommandOptions& options) {
    CLI::App* compact = app.add_subcommand("compact", "Reclaim the unused space in a virtual disk");
    compact->add_option("distro", options.name, "The distribution to compact");
    add_global_options(*compact, global);

    compact->add_flag("--all", options.all, "Every WSL2 distribution");
    compact->add_option("--file", options.file, "A loose .vhdx instead of a distribution's")
        ->option_text("PATH");
    compact->add_flag("--no-trim", options.no_trim, "Skip the fstrim step");
    compact->add_flag("--restart", options.restart,
                      "Start the distribution again afterwards if it was running");
    // Not a convenience flag. The utility VM holds every disk while any
    // distribution runs, so this stops all of them -- including whatever else
    // the user has open (D9).
    compact->add_flag("--shutdown", options.shutdown,
                      "Permit `wsl --shutdown` when something else holds the disk");
}

}  // namespace wsldisk::cli
