#include "completion_command.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>

#include "app.h"
#include "commands.h"

namespace wsldisk::cli {
namespace {

/// The names of a command's positional arguments, in declaration order.
[[nodiscard]] std::vector<std::string> positionals_of(const CLI::App& command) {
    std::vector<std::string> names;
    for (const CLI::Option* option : command.get_options()) {
        if (option->get_positional()) {
            names.push_back(option->get_name(true, false));
        }
    }
    return names;
}

/// Every option name a command accepts, long and short, in declaration order.
[[nodiscard]] std::vector<std::string> flags_of(const CLI::App& command) {
    std::vector<std::string> names;
    for (const CLI::Option* option : command.get_options()) {
        if (option->get_positional()) {
            continue;
        }
        for (const std::string& name : option->get_lnames()) {
            names.push_back("--" + name);
        }
        for (const std::string& name : option->get_snames()) {
            names.push_back("-" + name);
        }
    }
    return names;
}

/// The subcommands of `app`, in declaration order.
[[nodiscard]] std::vector<const CLI::App*> subcommands_of(const CLI::App& app) {
    std::vector<const CLI::App*> commands;
    for (const CLI::App* command : app.get_subcommands({})) {
        commands.push_back(command);
    }
    return commands;
}

/// `command:index` for every positional named `kind`, e.g. `relink:1` for the
/// path `relink` takes after the distribution.
///
/// Asked of the tree rather than listed: a command that grows a `distro` or a
/// `path` argument gets completion for it without anyone remembering to come
/// back here. The index matters -- `relink <distro> <path>` was the first
/// command with two, and offering distribution names for the path is worse
/// than offering nothing.
[[nodiscard]] bool names_a_path(std::string_view positional) {
    // `move <distro> <destination>` names its second argument for what it means
    // rather than for what it is. Completing it as a path is still the right
    // answer, so the kinds are listed here rather than assumed to be one word.
    return positional == "path" || positional == "destination";
}

[[nodiscard]] bool is_kind(std::string_view positional, std::string_view kind) {
    return kind == "path" ? names_a_path(positional) : positional == kind;
}

[[nodiscard]] std::vector<std::string> positional_slots(const CLI::App& app, std::string_view kind) {
    std::vector<std::string> slots;
    for (const CLI::App* command : subcommands_of(app)) {
        const std::vector<std::string> names = positionals_of(*command);
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (is_kind(names[index], kind)) {
                slots.push_back(std::format("{}:{}", command->get_name(), index));
            }
        }
    }
    return slots;
}

/// The zsh completer for a positional, or empty when it has none.
///
/// The same two names `positional_slots` recognises, in the form zsh wants.
[[nodiscard]] std::string_view zsh_completer_for(std::string_view positional) {
    if (positional == "distro") {
        return "_wsldisk_distros";
    }
    if (names_a_path(positional)) {
        return "_files";
    }
    return {};
}

/// The `_arguments` specs for a command's positionals, e.g. `'1: :_files'`.
///
/// Empty when the command has no positionals worth completing, which is what
/// lets the caller fall back to a command's own verbs.
[[nodiscard]] std::string zsh_positional_specs(const CLI::App& command) {
    std::string specs;
    const std::vector<std::string> positionals = positionals_of(command);
    for (std::size_t index = 0; index < positionals.size(); ++index) {
        const std::string_view completer = zsh_completer_for(positionals[index]);
        if (completer.empty()) {
            continue;
        }
        // zsh numbers positionals from 1.
        specs += std::format(" \\\n                        '{}: :{}'", index + 1, completer);
    }
    return specs;
}

/// Wraps text in single quotes for a POSIX shell, escaping any it contains.
///
/// Command descriptions are prose written by us -- "Tell one distribution's
/// filesystem to release unused blocks" -- and an apostrophe in one of them
/// closes the quote and breaks the script. Found exactly that way.
[[nodiscard]] std::string shell_quoted(std::string_view text) {
    std::string result = "'";
    for (const char character : text) {
        if (character == '\'') {
            // The only way out of a single-quoted shell string: close it, emit
            // an escaped quote, open it again.
            result += R"('\'')";
            continue;
        }
        result += character;
    }
    result += '\'';
    return result;
}

/// The names a command completes to after itself: its own subcommands, if it
/// has any. `config` has four verbs, and offering none of them would make the
/// completion worse than nothing there.
[[nodiscard]] std::vector<std::string> verbs_of(const CLI::App& command) {
    std::vector<std::string> names;
    for (const CLI::App* verb : command.get_subcommands({})) {
        names.push_back(verb->get_name());
    }
    return names;
}

[[nodiscard]] std::string joined(const std::vector<std::string>& items, std::string_view separator) {
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += separator;
        }
        text += item;
    }
    return text;
}

/// Names quoted for a PowerShell array literal.
[[nodiscard]] std::string powershell_list(const std::vector<std::string>& items) {
    std::vector<std::string> quoted;
    quoted.reserve(items.size());
    for (const std::string& item : items) {
        quoted.push_back("'" + item + "'");
    }
    return joined(quoted, ", ");
}

[[nodiscard]] std::string powershell_script(const CLI::App& app) {
    std::ostringstream out;
    out << "# PowerShell completions for wsldisk. Generated by `wsldisk completion powershell`;\n"
           "# regenerate it after upgrading rather than editing it.\n"
           "#\n"
           "# Add to your profile:\n"
           "#   wsldisk completion powershell | Out-String | Invoke-Expression\n\n";

    std::vector<std::string> names;
    out << "$script:WsldiskCommands = @{\n";
    for (const CLI::App* command : subcommands_of(app)) {
        names.push_back(command->get_name());
        // A command's own verbs complete alongside its flags.
        std::vector<std::string> candidates = verbs_of(*command);
        const std::vector<std::string> flags = flags_of(*command);
        candidates.insert(candidates.end(), flags.begin(), flags.end());
        out << "    '" << command->get_name() << "' = @(" << powershell_list(candidates) << ")\n";
    }
    out << "}\n\n";

    out << "$script:WsldiskGlobal = @(" << powershell_list(flags_of(app)) << ")\n\n";

    // Asked for at completion time, not baked in: the answer changes between
    // one invocation and the next.
    out << "function script:WsldiskDistros {\n"
           "    try {\n"
           "        (wsldisk list --json 2>$null) |\n"
           "            ForEach-Object { ($_ | ConvertFrom-Json).name } |\n"
           "            Where-Object { $_ }\n"
           "    } catch {\n"
           "        @()\n"
           "    }\n"
           "}\n\n";

    // Filenames, asked of the shell at completion time. `relink` takes a
    // path, and a completer that only knows distribution names would offer
    // distribution names for it.
    out << "function script:WsldiskPaths {\n"
           "    param($prefix)\n"
           "    try {\n"
           "        Get-ChildItem -Path \"$prefix*\" -ErrorAction SilentlyContinue |\n"
           "            ForEach-Object { $_.FullName }\n"
           "    } catch {\n"
           "        @()\n"
           "    }\n"
           "}\n\n";

    out << "$script:WsldiskDistroSlots = @(" << powershell_list(positional_slots(app, "distro")) << ")\n";
    out << "$script:WsldiskPathSlots = @(" << powershell_list(positional_slots(app, "path")) << ")\n\n";

    out << "Register-ArgumentCompleter -Native -CommandName wsldisk -ScriptBlock {\n"
           "    param($wordToComplete, $commandAst, $cursorPosition)\n\n"
           "    $words = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
           "    $bare = @($words | Select-Object -Skip 1 | Where-Object { -not $_.StartsWith('-') })\n"
           "    # The word being typed is already in the AST, so drop it: it is\n"
           "    # not a positional that has been supplied.\n"
           "    if ($bare.Count -gt 0 -and $bare[-1] -eq $wordToComplete) {\n"
           "        $bare = @($bare | Select-Object -First ($bare.Count - 1))\n"
           "    }\n"
           "    $command = if ($bare.Count -gt 0) { $bare[0] } else { $null }\n"
           "    $slot = \"${command}:$($bare.Count - 1)\"\n\n"
           "    $candidates = if (-not $command -or -not $script:WsldiskCommands.ContainsKey($command)) {\n"
           "        @($script:WsldiskCommands.Keys) + $script:WsldiskGlobal\n"
           "    } elseif ($wordToComplete.StartsWith('-')) {\n"
           "        $script:WsldiskCommands[$command]\n"
           "    } elseif ($script:WsldiskDistroSlots -contains $slot) {\n"
           "        WsldiskDistros\n"
           "    } elseif ($script:WsldiskPathSlots -contains $slot) {\n"
           "        WsldiskPaths $wordToComplete\n"
           "    } else {\n"
           "        $script:WsldiskCommands[$command]\n"
           "    }\n\n"
           "    $candidates | Where-Object { $_ -like \"$wordToComplete*\" } | Sort-Object |\n"
           "        ForEach-Object {\n"
           "            [System.Management.Automation.CompletionResult]::new(\n"
           "                $_, $_, 'ParameterValue', $_)\n"
           "        }\n"
           "}\n";
    return out.str();
}

[[nodiscard]] std::string bash_script(const CLI::App& app) {
    std::ostringstream out;
    out << "# bash completions for wsldisk. Generated by `wsldisk completion bash`;\n"
           "# regenerate it after upgrading rather than editing it.\n"
           "#\n"
           "# Add to your profile:\n"
           "#   source <(wsldisk completion bash)\n\n";

    std::vector<std::string> names;
    for (const CLI::App* command : subcommands_of(app)) {
        names.push_back(command->get_name());
    }

    out << "_wsldisk() {\n";
    out << "    local commands='" << joined(names, " ") << "'\n";
    out << "    local global='" << joined(flags_of(app), " ") << "'\n";
    // `command:index` pairs rather than bare command names: which
    // positional is being typed decides what to offer for it.
    out << "    local distro_slots=' " << joined(positional_slots(app, "distro"), " ") << " '\n";
    out << "    local path_slots=' " << joined(positional_slots(app, "path"), " ") << " '\n\n";

    out << "    local cur prev command word\n"
           "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
           "    command=''\n"
           "    local positional=-1\n"
           "    for word in \"${COMP_WORDS[@]:1:COMP_CWORD-1}\"; do\n"
           "        case \"$word\" in\n"
           "            -*) ;;\n"
           "            *)\n"
           "                if [[ -z \"$command\" ]]; then\n"
           "                    command=\"$word\"\n"
           "                else\n"
           "                    positional=$((positional + 1))\n"
           "                fi\n"
           "                ;;\n"
           "        esac\n"
           "    done\n"
           "    positional=$((positional + 1))\n\n";

    out << "    local flags=''\n"
           "    case \"$command\" in\n";
    for (const CLI::App* command : subcommands_of(app)) {
        // A command's own verbs complete alongside its flags: `config <TAB>`
        // offering nothing would be worse than no completion at all there.
        std::vector<std::string> candidates = verbs_of(*command);
        const std::vector<std::string> flags = flags_of(*command);
        candidates.insert(candidates.end(), flags.begin(), flags.end());
        out << "        " << command->get_name() << ") flags='" << joined(candidates, " ") << "' ;;\n";
    }
    out << "        *) COMPREPLY=($(compgen -W \"$commands $global\" -- \"$cur\")); return ;;\n"
           "    esac\n\n";

    // Asked for at completion time, not baked in.
    out << "    local slot=\"$command:$positional\"\n"
           "    if [[ \"$cur\" != -* ]] && [[ \"$distro_slots\" == *\" $slot \"* ]]; then\n"
           "        local distros\n"
           "        distros=$(wsldisk list --json 2>/dev/null |\n"
           "            sed -n 's/.*\"name\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p')\n"
           "        COMPREPLY=($(compgen -W \"$distros\" -- \"$cur\"))\n"
           "        return\n"
           "    fi\n\n"
           "    if [[ \"$cur\" != -* ]] && [[ \"$path_slots\" == *\" $slot \"* ]]; then\n"
           "        COMPREPLY=($(compgen -f -- \"$cur\"))\n"
           "        return\n"
           "    fi\n\n"
           "    COMPREPLY=($(compgen -W \"$flags\" -- \"$cur\"))\n"
           "}\n\n"
           "complete -F _wsldisk wsldisk\n";
    return out.str();
}

[[nodiscard]] std::string zsh_script(const CLI::App& app) {
    std::ostringstream out;
    out << "#compdef wsldisk\n"
           "# zsh completions for wsldisk. Generated by `wsldisk completion zsh`;\n"
           "# regenerate it after upgrading rather than editing it.\n"
           "#\n"
           "# Add to your profile:\n"
           "#   source <(wsldisk completion zsh)\n\n";

    // A custom delimiter: the script contains `)"`, which would end a plain
    // raw string literal in the middle of the zsh substitution.
    out << R"ZSH(_wsldisk_distros() {
    local -a names
    names=(${(f)"$(wsldisk list --json 2>/dev/null |
        sed -n 's/.*"name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"})
    _describe 'distribution' names
}

)ZSH";

    out << "_wsldisk() {\n"
           "    local -a commands\n"
           "    commands=(\n";
    for (const CLI::App* command : subcommands_of(app)) {
        // The description goes in the menu, which is most of why zsh completion
        // is worth generating rather than listing names. Quoted, because those
        // descriptions are prose and some of them contain an apostrophe.
        out << "        " << shell_quoted(command->get_name() + ":" + command->get_description()) << '\n';
    }
    out << "    )\n\n";

    out << "    _arguments -C \\\n";
    for (const std::string& flag : flags_of(app)) {
        out << "        '" << flag << "' \\\n";
    }
    out << "        '1: :{_describe \"command\" commands}' \\\n"
           "        '*:: :->args'\n\n";

    out << "    case $state in\n"
           "        args)\n"
           "            case $words[1] in\n";
    for (const CLI::App* command : subcommands_of(app)) {
        out << "                " << command->get_name() << ")\n";
        out << "                    _arguments";
        for (const std::string& flag : flags_of(*command)) {
            out << " \\\n                        '" << flag << "'";
        }
        if (const std::string positionals = zsh_positional_specs(*command); !positionals.empty()) {
            out << positionals;
        } else if (const std::vector<std::string> verbs = verbs_of(*command); !verbs.empty()) {
            // A command with verbs of its own -- `config get`, `config set` --
            // completes to those rather than to nothing.
            out << " \\\n                        '1: :(" << joined(verbs, " ") << ")'";
        }
        out << "\n                    ;;\n";
    }
    out << "            esac\n"
           "            ;;\n"
           "    esac\n"
           "}\n\n"
           "_wsldisk \"$@\"\n";
    return out.str();
}

}  // namespace

std::vector<std::string> completion_shells() {
    return {"powershell", "bash", "zsh"};
}

std::string generate_completion(const CLI::App& app, std::string_view shell) {
    if (shell == "powershell") {
        return powershell_script(app);
    }
    if (shell == "bash") {
        return bash_script(app);
    }
    return zsh_script(app);
}

int run_completion(const CompletionOptions& options, const GlobalOptions& global, std::ostream& out,
                   std::ostream& err) {
    const std::vector<std::string> shells = completion_shells();
    if (std::ranges::find(shells, options.shell) == shells.end()) {
        return report(Error{ErrorCode::Usage, std::format("there is no completion for `{}`", options.shell),
                            std::format("the shells are: {}", joined(shells, ", "))},
                      global, out, err);
    }

    // A throwaway tree, built by the same function that builds the real one, so
    // the script can only ever describe the flags that exist.
    CLI::App app{"Compact, shrink, move, inspect and snapshot WSL2 virtual disks", "wsldisk"};
    CommandOptions unused;
    add_all_commands(app, unused);

    out << generate_completion(app, options.shell);
    return exit_code_success;
}

void add_completion_command(CLI::App& app, GlobalOptions& global, CompletionOptions& options) {
    CLI::App* completion = app.add_subcommand("completion", "Print a shell completion script for wsldisk");
    // No `--json`: this prints a shell script to be sourced, and there is
    // nothing for the flag to mean. It used to be accepted and ignored, so
    // `wsldisk completion bash --json` handed a JSON reader a bash function.
    add_global_options(*completion, global, /*machine_readable=*/false);
    completion->add_option("shell", options.shell, "powershell, bash or zsh")
        ->option_text("SHELL")
        ->required();
}

}  // namespace wsldisk::cli
