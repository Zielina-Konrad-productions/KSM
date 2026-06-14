#include "main.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace ftxui;

struct Module {
    std::string title;
    std::string tool;
    std::vector<std::string> args;
    std::string description;
    bool needsRoot = false;
};

struct Category {
    std::string title;
    std::vector<Module> modules;
};

void version() {
    std::cout << BLUE << "kcontrol component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kcontrol [options]\n";
    std::cout << "FTXUI based KSM control center.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Left/Right    Change category\n";
    std::cout << "  Up/Down       Move in module list\n";
    std::cout << "  Enter         Launch selected module\n";
    std::cout << "  F1/?          Help overlay\n";
    std::cout << "  q             Exit\n";
}

std::vector<Category> categories() {
    return {
        {
            "Overview",
            {
                {"KSM Home", "khome", {}, "Browse KSM help pages", false},
                {"Interactive Home", "khome", {"-ui"}, "Open interactive khome", false},
                {"System Info", "ksysinfo", {}, "Live system dashboard", false},
                {"Upgrade KSM", "kupgr", {}, "Update from GitHub Releases", true}
            }
        },
        {
            "Users",
            {
                {"Add Users", "kuseradd", {}, "Create one or many users", true},
                {"Modify Users", "kusermod", {}, "Edit user account settings", true},
                {"Delete Users", "kuserdel", {}, "Remove user accounts", true},
                {"Add Groups", "kgroupadd", {}, "Create groups", true},
                {"Modify Groups", "kgroupmod", {}, "Edit groups and members", true},
                {"Delete Groups", "kgroupdel", {}, "Remove groups", true}
            }
        },
        {
            "System",
            {
                {"Services", "kserv", {}, "Start, stop and enable systemd services", false},
                {"Permissions", "kperm", {}, "Browse files and edit chmod/chown", true},
                {"Network Config", "knetcfg", {}, "Configure network interfaces", true}
            }
        },
        {
            "Security",
            {
                {"SSH Config", "kssh", {}, "Configure sshd safely with backup", true},
                {"Firewall", "kfirewall", {}, "Manage ufw or firewalld rules", true}
            }
        },
        {
            "Maintenance",
            {
                {"Uninstall KSM", "kuninstall", {}, "Remove KSM installation", true},
                {"Updater", "kupgr", {}, "Stable release updater", true},
                {"Experimental Updater", "kupgr", {"-ex"}, "Prerelease and snapshot updater", true}
            }
        }
    };
}

std::string tool_path(const std::string& tool) {
    const std::string installed = "/opt/KSM/bin/" + tool;
    if (access(installed.c_str(), X_OK) == 0) return installed;
    return tool;
}

std::string command_text(const Module& module) {
    std::string result = module.tool;
    for (const auto& arg : module.args) result += " " + arg;
    return result;
}

std::string fit_text(const std::string& value, size_t width) {
    if (value.size() <= width) return value;
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

Element module_line(const Module& module, bool active) {
    auto line = hbox({
        text(active ? ">" : " ") | size(WIDTH, EQUAL, 2),
        text(fit_text(module.title, 22)) | size(WIDTH, EQUAL, 23),
        text(module.description) | flex
    });

    if (active) return line | bold | color(Color::White) | bgcolor(Color::Blue);
    if (module.needsRoot) return line | color(Color::Yellow);
    return line | color(Color::Cyan);
}

Element category_line(const Category& category, bool active) {
    auto line = hbox({
        text(active ? ">" : " ") | size(WIDTH, EQUAL, 2),
        text(category.title) | flex
    });
    if (active) return line | bold | color(Color::White) | bgcolor(Color::Blue);
    return line | color(Color::Cyan);
}

Element action_button(const std::string& key, const std::string& label) {
    return hbox({
        text(" " + key + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
        text(" " + label + " ")
    });
}

Element help_overlay() {
    return vbox({
        text("KSM Control Center") | bold | color(Color::Cyan) | hcenter,
        separator(),
        text("Left/Right  Change category"),
        text("Up/Down     Move through modules"),
        text("Enter       Launch selected module"),
        text("Tab         Next category"),
        text("F1/?        Toggle this help"),
        text("q/Esc       Exit"),
        separator(),
        text("Yellow modules may ask for sudo.") | color(Color::Yellow),
    }) | border | size(WIDTH, LESS_THAN, 62) | center;
}

Element render_ui(
    const std::vector<Category>& items,
    int categoryIndex,
    int moduleIndex,
    const std::string& message,
    bool showHelp
) {
    const auto& category = items[categoryIndex];
    const auto& module = category.modules[moduleIndex];

    Elements categoryRows;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        categoryRows.push_back(category_line(items[i], i == categoryIndex));
    }

    Elements moduleRows;
    for (int i = 0; i < static_cast<int>(category.modules.size()); ++i) {
        moduleRows.push_back(module_line(category.modules[i], i == moduleIndex));
    }

    auto categoriesPanel = vbox(std::move(categoryRows)) |
        borderStyled(Color::Blue) |
        size(WIDTH, EQUAL, 26);

    auto modulesPanel = vbox(std::move(moduleRows)) |
        borderStyled(Color::Blue) |
        flex;

    auto detailsPanel = vbox({
        hbox({
            text(module.title) | bold | color(Color::Cyan),
            filler(),
            text(module.needsRoot ? "sudo/root" : "normal user") |
                color(module.needsRoot ? Color::Yellow : Color::Green)
        }),
        separator(),
        hbox({text("Command     ") | dim, text(command_text(module)) | color(Color::Cyan)}),
        hbox({text("Description ") | dim, paragraph(module.description) | flex}),
        hbox({text("Category    ") | dim, text(category.title)})
    }) | borderStyled(Color::Blue);

    auto status = message.empty()
        ? text("Ready") | dim
        : text(message) | color(Color::Yellow);

    auto footer = hbox({
        action_button("Enter", "Launch"),
        text("  "),
        action_button("<-/->", "Category"),
        text("  "),
        action_button("Up/Down", "Module"),
        text("  "),
        action_button("F1", "Help"),
        text("  "),
        action_button("q", "Exit"),
        filler(),
        status
    }) | borderStyled(Color::Blue);

    auto document = vbox({
        text("KASTIUSZ SYSTEM MANAGER") | bold | color(Color::Cyan) | hcenter,
        text("FTXUI Control Center - YaST-style modules, KSM flow") | dim | hcenter,
        separator(),
        hbox({
            vbox({
                text(" Categories ") | bold | color(Color::Cyan),
                categoriesPanel | flex
            }),
            text("  "),
            vbox({
                text(" Modules: " + category.title + " ") | bold | color(Color::Cyan),
                modulesPanel | flex
            }) | flex
        }) | flex,
        detailsPanel,
        footer
    }) | borderStyled(Color::Cyan);

    if (showHelp) {
        return dbox({
            document,
            help_overlay()
        });
    }
    return document;
}

int run_program(const Module& module) {
    std::cout << BLUE << "KSM Control Center" << RESET << '\n';
    std::cout << CYAN << "[*]" << RESET << " Launching " << module.title << "...\n\n" << std::flush;

    const bool useSudo = module.needsRoot && geteuid() != 0;
    std::vector<std::string> args;
    if (useSudo) {
        args.push_back("sudo");
        args.push_back(tool_path(module.tool));
    } else {
        args.push_back(tool_path(module.tool));
    }
    args.push_back("--panel");
    args.insert(args.end(), module.args.begin(), module.args.end());

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << RED << "ERROR:" << RESET << " fork failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (pid == 0) {
        setenv("KSM_PANEL", "1", 1);
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::cerr << RED << "ERROR:" << RESET << " could not execute " << argv[0] << ": " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << RED << "ERROR:" << RESET << " waitpid failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    std::cout << '\n' << CYAN << "[*]" << RESET << " Returned to kcontrol. Press Enter..." << std::flush;
    std::string ignored;
    std::getline(std::cin, ignored);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

void clamp_module(const std::vector<Category>& items, int categoryIndex, int& moduleIndex) {
    const int maxIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
    if (moduleIndex > maxIndex) moduleIndex = maxIndex;
    if (moduleIndex < 0) moduleIndex = 0;
}

int run_tui() {
    const auto items = categories();
    int categoryIndex = 0;
    int moduleIndex = 0;
    std::string message;
    bool showHelp = false;
    bool shouldQuit = false;
    bool shouldLaunch = false;

    while (!shouldQuit) {
        shouldLaunch = false;
        auto screen = ScreenInteractive::Fullscreen();
        auto exitLoop = screen.ExitLoopClosure();

        auto root = Renderer([&] {
            return render_ui(items, categoryIndex, moduleIndex, message, showHelp);
        });

        root = CatchEvent(root, [&](Event event) {
            if (event == Event::Character("q") || event == Event::Character("Q") || event == Event::Escape) {
                shouldQuit = true;
                exitLoop();
                return true;
            }
            if (event == Event::F1 || event == Event::Character("?")) {
                showHelp = !showHelp;
                return true;
            }
            if (showHelp) {
                showHelp = false;
                return true;
            }
            if (event == Event::ArrowLeft) {
                --categoryIndex;
                if (categoryIndex < 0) categoryIndex = static_cast<int>(items.size()) - 1;
                clamp_module(items, categoryIndex, moduleIndex);
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Tab) {
                ++categoryIndex;
                if (categoryIndex >= static_cast<int>(items.size())) categoryIndex = 0;
                clamp_module(items, categoryIndex, moduleIndex);
                return true;
            }
            if (event == Event::ArrowUp) {
                --moduleIndex;
                if (moduleIndex < 0) moduleIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
                return true;
            }
            if (event == Event::ArrowDown) {
                ++moduleIndex;
                if (moduleIndex >= static_cast<int>(items[categoryIndex].modules.size())) moduleIndex = 0;
                return true;
            }
            if (event == Event::Return) {
                shouldLaunch = true;
                exitLoop();
                return true;
            }
            return false;
        });

        screen.Loop(root);
        if (shouldLaunch) {
            const auto& module = items[categoryIndex].modules[moduleIndex];
            const int code = run_program(module);
            message = code == 0 ? "" : module.title + " exited with code " + std::to_string(code) + ".";
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            version();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            help();
            return 0;
        }
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kcontrol --help' to list options.\n";
        return 1;
    }
    return run_tui();
}
