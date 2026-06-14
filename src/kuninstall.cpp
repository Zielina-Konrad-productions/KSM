#include "main.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr const char* kTargetPath = "/opt/KSM";
constexpr const char* kBinPath = "/usr/bin";

enum class Key {
    Up,
    Down,
    Enter,
    Escape,
    CtrlC,
    Character,
    Unknown
};

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

struct Options {
    bool removeInstallDir = true;
    bool removeCommandLinks = true;
    bool force = false;
    std::string message;
};

struct ProcessResult {
    int exitCode = 1;
};

class Terminal {
public:
    Terminal() {
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldTerm_) == 0) {
            termios raw = oldTerm_;
            raw.c_lflag &= ~(ECHO | ICANON);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            rawEnabled_ = (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
        }
        std::cout << "\033[?25l";
    }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    ~Terminal() {
        restore();
    }

    void restore() {
        if (restored_) return;
        std::cout << "\033[?25h\033[0m" << std::flush;
        if (rawEnabled_) tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
        restored_ = true;
    }

private:
    termios oldTerm_ {};
    bool rawEnabled_ = false;
    bool restored_ = false;
};

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

void banner() {
    std::cout << BLUE;
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "              kuninstall\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kuninstall component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "sudo kuninstall [options]\n";
    std::cout << "Interactive terminal GUI for uninstalling KSM.\n\n";
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  --force, -f     Skip confirmation\n";
    std::cout << "  --help, -h      Show this help\n";
    std::cout << "  --version, -v   Show version information\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down         Move\n";
    std::cout << "  Enter           Toggle option or run action\n";
    std::cout << "  q               Cancel\n";
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
    if (c == '\n' || c == '\r') return {Key::Enter, '\0'};
    if (c == 27) {
        char second = '\0';
        char third = '\0';
        if (!read_byte_timeout(second, 50)) return {Key::Escape, '\0'};
        if (second != '[' || !read_byte_timeout(third, 50)) return {Key::Escape, '\0'};
        if (third == 'A') return {Key::Up, '\0'};
        if (third == 'B') return {Key::Down, '\0'};
        return {Key::Unknown, '\0'};
    }
    if (c >= 32 && c <= 126) return {Key::Character, c};
    return {Key::Unknown, '\0'};
}

std::string yes_no(bool value) {
    return value ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
}

void draw_row(int row, int cursor, const std::string& text, bool danger = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (danger) std::cout << RED;
    std::cout << text << RESET << '\n';
}

void draw(const Options& options, int cursor) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Enter:" << RESET << " toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    std::cout << BOLD << "Uninstall settings" << RESET << '\n';
    std::cout << "  Install path: " << CYAN << kTargetPath << RESET << '\n';
    std::cout << "  Link path   : " << CYAN << kBinPath << RESET << "\n\n";

    draw_row(0, cursor, "Remove command links " + yes_no(options.removeCommandLinks));
    draw_row(1, cursor, "Remove /opt/KSM      " + yes_no(options.removeInstallDir));
    draw_row(2, cursor, "Force no prompt      " + yes_no(options.force));
    draw_row(3, cursor, "Start uninstall      Enter", true);
    draw_row(4, cursor, "Cancel               Enter or q");

    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
    std::cout << std::flush;
}

ProcessResult run_process(const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid < 0) return {1};

    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return {1};
    if (WIFEXITED(status)) return {WEXITSTATUS(status)};
    if (WIFSIGNALED(status)) return {128 + WTERMSIG(status)};
    return {1};
}

bool confirm_uninstall(const Options& options) {
    if (options.force) return true;

    clear_screen();
    banner();
    std::cout << RED << "Ready to uninstall KSM" << RESET << "\n\n";
    std::cout << "Remove command links: " << (options.removeCommandLinks ? "yes" : "no") << '\n';
    std::cout << "Remove /opt/KSM     : " << (options.removeInstallDir ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to uninstall, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

std::vector<std::string> commands() {
    return {"ksm", "khome", "kupgr", "kuninstall", "ksysinfo", "kserv", "kperm", "kssh", "kfirewall", "kgroupadd", "kgroupmod", "kgroupdel", "kuseradd", "kusermod", "kuserdel", "knetcfg"};
}

bool remove_command_links() {
    for (const auto& command : commands()) {
        const fs::path link = fs::path(kBinPath) / command;
        const fs::path expected = fs::path(kTargetPath) / "bin" / command;

        std::error_code ec;
        const auto status = fs::symlink_status(link, ec);
        if (ec || !fs::exists(status)) continue;

        if (!fs::is_symlink(status)) {
            std::cout << YELLOW << "[!]" << RESET << " Skipping non-symlink " << link << '\n';
            continue;
        }

        const fs::path target = fs::read_symlink(link, ec);
        if (ec || target != expected) {
            std::cout << YELLOW << "[!]" << RESET << " Skipping " << link << " (not a KSM link)\n";
            continue;
        }

        std::cout << CYAN << "[*]" << RESET << " Removing " << link << "...\n";
        fs::remove(link, ec);
        if (ec) {
            std::cout << RED << "[x]" << RESET << " Failed to remove " << link << ": " << ec.message() << '\n';
            return false;
        }
        std::cout << GREEN << "[+]" << RESET << " Removed " << link << '\n';
    }
    return true;
}

bool remove_install_dir() {
    std::error_code ec;
    if (!fs::exists(kTargetPath, ec)) {
        std::cout << YELLOW << "[!]" << RESET << " " << kTargetPath << " does not exist.\n";
        return true;
    }

    std::cout << CYAN << "[*]" << RESET << " Removing " << kTargetPath << "...\n";
    fs::remove_all(kTargetPath, ec);
    if (ec) {
        std::cout << RED << "[x]" << RESET << " Failed to remove " << kTargetPath << ": " << ec.message() << '\n';
        return false;
    }
    std::cout << GREEN << "[+]" << RESET << " Removed " << kTargetPath << '\n';
    return true;
}

int run_uninstall(Terminal& terminal, const Options& options) {
    clear_screen();
    banner();
    terminal.restore();

    bool ok = true;
    if (options.removeCommandLinks) {
        ok = ok && remove_command_links();
    }
    if (options.removeInstallDir) {
        ok = ok && remove_install_dir();
    }

    std::cout << '\n';
    if (ok) {
        std::cout << GREEN << "[+]" << RESET << " KSM uninstall complete.\n";
        return 0;
    }

    std::cout << RED << "[x]" << RESET << " KSM uninstall failed.\n";
    return 1;
}

void move_cursor(int& cursor, int delta) {
    constexpr int maxRow = 4;
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

int run_tui(Options options) {
    if (geteuid() != 0) {
        banner();
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    Terminal terminal;
    int cursor = 3;

    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1);
        } else if (key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) {
                options.removeCommandLinks = !options.removeCommandLinks;
            } else if (cursor == 1) {
                options.removeInstallDir = !options.removeInstallDir;
            } else if (cursor == 2) {
                options.force = !options.force;
            } else if (cursor == 3) {
                if (!options.removeCommandLinks && !options.removeInstallDir) {
                    options.message = "Select at least one uninstall action.";
                } else if (confirm_uninstall(options)) {
                    return run_uninstall(terminal, options);
                }
            } else if (cursor == 4) {
                return 0;
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;

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
        if (arg == "--force" || arg == "-f") {
            options.force = true;
            continue;
        }

        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kuninstall --help' to list options.\n";
        return 1;
    }

    return run_tui(options);
}
