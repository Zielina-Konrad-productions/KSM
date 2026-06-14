#include "main.h"
#include "tui.h"

#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class Key { Up, Down, Left, Right, Tab, Enter, Escape, CtrlC, Character, Unknown };

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

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

class Terminal {
public:
    Terminal() {
        enable_raw();
        ksm_tui::enter_screen();
    }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    ~Terminal() {
        restore();
        ksm_tui::leave_screen();
    }

    void restore() {
        if (rawEnabled_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
            rawEnabled_ = false;
        }
    }

    void enable_raw() {
        if (!rawEnabled_ && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldTerm_) == 0) {
            termios raw = oldTerm_;
            raw.c_lflag &= ~(ECHO | ICANON);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            rawEnabled_ = (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
        }
    }

private:
    termios oldTerm_ {};
    bool rawEnabled_ = false;
};

void clear_screen() {
    ksm_tui::clear();
}

void banner() {
    std::cout << BLUE << "Kastiusz System Manager" << RESET << '\n';
}

void version() {
    std::cout << BLUE << "kcontrol component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kcontrol [options]\n";
    std::cout << "YaST-style interactive KSM control center, but lighter and meaner.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Left/Right    Change category\n";
    std::cout << "  Up/Down       Move in module list\n";
    std::cout << "  Enter         Launch selected module\n";
    std::cout << "  q             Exit\n";
}

bool read_byte_timeout(char& c, int milliseconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval timeout {};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
    if (ready <= 0) return false;
    return read(STDIN_FILENO, &c, 1) == 1;
}

KeyPress read_key() {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) != 1) return {Key::Unknown, '\0'};
    if (c == 3) return {Key::CtrlC, '\0'};
    if (c == '\t') return {Key::Tab, '\0'};
    if (c == '\n' || c == '\r') return {Key::Enter, '\0'};
    if (c == 27) {
        char second = '\0';
        char third = '\0';
        if (!read_byte_timeout(second, 50)) return {Key::Escape, '\0'};
        if (second != '[' || !read_byte_timeout(third, 50)) return {Key::Escape, '\0'};
        if (third == 'A') return {Key::Up, '\0'};
        if (third == 'B') return {Key::Down, '\0'};
        if (third == 'C') return {Key::Right, '\0'};
        if (third == 'D') return {Key::Left, '\0'};
        return {Key::Unknown, '\0'};
    }
    if (c >= 32 && c <= 126) return {Key::Character, c};
    return {Key::Unknown, '\0'};
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

int run_program(Terminal& terminal, const Module& module) {
    terminal.restore();
    clear_screen();
    banner();
    std::cout << CYAN << "[*]" << RESET << " Launching " << module.title << "...\n\n" << std::flush;

    const bool useSudo = module.needsRoot && geteuid() != 0;
    std::vector<std::string> args;
    if (useSudo) {
        args.push_back("sudo");
        args.push_back(tool_path(module.tool));
    } else {
        args.push_back(tool_path(module.tool));
    }
    args.insert(args.end(), module.args.begin(), module.args.end());

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << RED << "ERROR:" << RESET << " fork failed: " << std::strerror(errno) << '\n';
        terminal.enable_raw();
        return 1;
    }

    if (pid == 0) {
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
        terminal.enable_raw();
        return 1;
    }

    std::cout << '\n' << CYAN << "[*]" << RESET << " Returned to kcontrol. Press any key.\n" << std::flush;
    terminal.enable_raw();
    read_key();
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

void draw(const std::vector<Category>& items, int categoryIndex, int moduleIndex, const std::string& message) {
    clear_screen();
    const auto& category = items[categoryIndex];
    const Module& selected = category.modules[moduleIndex];

    ksm_tui::border(80);
    ksm_tui::row(ksm_tui::center("KSM CONTROL CENTER", 78), 80, BOLD + CYAN);
    ksm_tui::row(ksm_tui::center("YaST-style configuration modules, KSM speed", 78), 80, DIM);
    ksm_tui::split_border(23, 57);
    ksm_tui::split_row(" Categories", " Modules: " + category.title, 23, 57, BOLD, BOLD);
    ksm_tui::split_border(23, 57);

    constexpr int visibleRows = 12;
    for (int row = 0; row < visibleRows; ++row) {
        std::string left;
        std::string right;
        std::string leftColor;
        std::string rightColor;

        if (row < static_cast<int>(items.size())) {
            const bool active = row == categoryIndex;
            left = ksm_tui::selected(items[row].title, active);
            leftColor = active ? BLUE : "";
        }

        if (row < static_cast<int>(category.modules.size())) {
            const auto& module = category.modules[row];
            const bool active = row == moduleIndex;
            right = ksm_tui::selected(module.title, active);
            if (module.title.size() < 21) right += std::string(21 - module.title.size(), ' ');
            right += " " + module.description;
            rightColor = active ? BLUE : (module.needsRoot ? YELLOW : "");
        }

        ksm_tui::split_row(left, right, 23, 57, leftColor, rightColor);
    }

    ksm_tui::split_border(23, 57);
    ksm_tui::row(" Selected: " + selected.title + " -> " + selected.tool, 80, CYAN);
    ksm_tui::row(" Description: " + selected.description, 80);
    ksm_tui::row(std::string(" Privilege: ") + (selected.needsRoot ? "sudo/root required" : "normal user"), 80, selected.needsRoot ? YELLOW : GREEN);
    if (!message.empty()) ksm_tui::row(" Status: " + message, 80, YELLOW);
    else ksm_tui::row(" Status: ready", 80, DIM);
    ksm_tui::border(80);
    ksm_tui::row(" [Enter] Launch   [Left/Right] Category   [Up/Down] Module   [Tab] Next   [q] Exit", 80, BOLD);
    ksm_tui::border(80);
    std::cout << std::flush;
}

void clamp_module(const std::vector<Category>& items, int categoryIndex, int& moduleIndex) {
    const int maxIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
    if (moduleIndex > maxIndex) moduleIndex = maxIndex;
    if (moduleIndex < 0) moduleIndex = 0;
}

int run_tui() {
    const auto items = categories();
    Terminal terminal;
    int categoryIndex = 0;
    int moduleIndex = 0;
    std::string message;

    while (true) {
        draw(items, categoryIndex, moduleIndex, message);
        const KeyPress key = read_key();
        message.clear();

        if (key.key == Key::CtrlC || key.key == Key::Escape ||
            (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Left) {
            --categoryIndex;
            if (categoryIndex < 0) categoryIndex = static_cast<int>(items.size()) - 1;
            clamp_module(items, categoryIndex, moduleIndex);
        } else if (key.key == Key::Right || key.key == Key::Tab) {
            ++categoryIndex;
            if (categoryIndex >= static_cast<int>(items.size())) categoryIndex = 0;
            clamp_module(items, categoryIndex, moduleIndex);
        } else if (key.key == Key::Up) {
            --moduleIndex;
            if (moduleIndex < 0) moduleIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
        } else if (key.key == Key::Down) {
            ++moduleIndex;
            if (moduleIndex >= static_cast<int>(items[categoryIndex].modules.size())) moduleIndex = 0;
        } else if (key.key == Key::Enter) {
            const auto& module = items[categoryIndex].modules[moduleIndex];
            const int code = run_program(terminal, module);
            if (code != 0) message = module.title + " exited with code " + std::to_string(code) + ".";
        }
    }
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
