#include "main.h"

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

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

struct UserEntry {
    std::string name;
    uid_t uid = 0;
    std::string home;
    std::string shell;
    bool selected = false;
};

struct Options {
    bool removeHome = true;
    bool force = false;
    bool showSystemUsers = false;
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
        std::cout << "\033[?25h\033[0m" << std::flush;
        if (rawEnabled_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
        }
    }

private:
    termios oldTerm_ {};
    bool rawEnabled_ = false;
};

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

void banner() {
    std::cout << BLUE;
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "              kuserdel\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kuserdel component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kuserdel [options]\n";
    std::cout << "Interactive terminal GUI for deleting users.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Enter         Select user, toggle option, or run action\n";
    std::cout << "  q             Cancel\n";
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

bool is_normal_user(const UserEntry& user) {
    return user.uid >= 1000 && user.name != "nobody";
}

std::vector<UserEntry> read_users(bool showSystemUsers) {
    std::vector<UserEntry> users;
    setpwent();
    while (struct passwd* pw = getpwent()) {
        UserEntry user;
        user.name = pw->pw_name ? pw->pw_name : "";
        user.uid = pw->pw_uid;
        user.home = pw->pw_dir ? pw->pw_dir : "";
        user.shell = pw->pw_shell ? pw->pw_shell : "";

        if (user.name.empty() || user.name == "root") continue;
        if (!showSystemUsers && !is_normal_user(user)) continue;
        users.push_back(user);
    }
    endpwent();

    std::sort(users.begin(), users.end(), [](const UserEntry& a, const UserEntry& b) {
        return a.name < b.name;
    });
    return users;
}

int selected_count(const std::vector<UserEntry>& users) {
    return static_cast<int>(std::count_if(users.begin(), users.end(), [](const UserEntry& user) {
        return user.selected;
    }));
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

void draw(const std::vector<UserEntry>& users, const Options& options, int cursor, int offset) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Enter:" << RESET << " select/toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n";
    std::cout << "Selected users: " << GREEN << selected_count(users) << RESET << "\n\n";

    constexpr int pageSize = 12;
    const int end = std::min<int>(static_cast<int>(users.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        const auto& user = users[i];
        std::ostringstream row;
        row << (user.selected ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
        row << user.name << "  uid:" << user.uid << "  home:" << (user.home.empty() ? "-" : user.home);
        draw_row(i, cursor, row.str());
    }

    if (users.empty()) std::cout << "  No users to show.\n";

    const int removeHomeRow = static_cast<int>(users.size());
    const int forceRow = removeHomeRow + 1;
    const int showSystemRow = removeHomeRow + 2;
    const int deleteRow = removeHomeRow + 3;
    const int cancelRow = removeHomeRow + 4;

    std::cout << '\n';
    draw_row(removeHomeRow, cursor, "Remove home directory  " + yes_no(options.removeHome));
    draw_row(forceRow, cursor, "Force delete            " + yes_no(options.force));
    draw_row(showSystemRow, cursor, "Show system users      " + yes_no(options.showSystemUsers));
    draw_row(deleteRow, cursor, "Delete selected        Enter", true);
    draw_row(cancelRow, cursor, "Cancel                 Enter or q");

    if (!options.message.empty()) std::cout << '\n' << YELLOW << options.message << RESET << '\n';
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

bool command_exists(const std::string& command) {
    const ProcessResult result = run_process({"sh", "-c", "command -v " + command + " >/dev/null 2>&1"});
    return result.exitCode == 0;
}

std::vector<UserEntry> selected_users(const std::vector<UserEntry>& users) {
    std::vector<UserEntry> out;
    for (const auto& user : users) if (user.selected) out.push_back(user);
    return out;
}

bool confirm_delete(const std::vector<UserEntry>& users, const Options& options) {
    clear_screen();
    banner();
    std::cout << RED << "Ready to delete users" << RESET << "\n\n";
    for (const auto& user : selected_users(users)) {
        std::cout << "  - " << user.name << " (uid:" << user.uid << ", home:" << user.home << ")\n";
    }
    std::cout << "\nRemove home: " << (options.removeHome ? "yes" : "no") << '\n';
    std::cout << "Force delete: " << (options.force ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to delete, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int delete_user(const UserEntry& user, const Options& options) {
    std::vector<std::string> args = {"userdel"};
    if (options.force) args.push_back("-f");
    if (options.removeHome) args.push_back("-r");
    args.push_back(user.name);
    return run_process(args).exitCode;
}

int run_deletion(const std::vector<UserEntry>& users, const Options& options) {
    clear_screen();
    banner();

    int failures = 0;
    for (const auto& user : selected_users(users)) {
        std::cout << CYAN << "[*]" << RESET << " Deleting " << user.name << "...\n";
        const int result = delete_user(user, options);
        if (result == 0) {
            std::cout << GREEN << "[+]" << RESET << " Deleted " << user.name << '\n';
        } else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " Failed " << user.name << " (exit " << result << ")\n";
        }
    }

    std::cout << '\n';
    std::cout << (failures == 0 ? GREEN + "Done." + RESET : RED + "Done with failures: " + std::to_string(failures) + RESET) << '\n';
    std::cout << "Press any key to exit.\n";
    read_key();
    return failures == 0 ? 0 : 1;
}

void move_cursor(int& cursor, int delta, int maxRow) {
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

int run_tui() {
    if (geteuid() != 0) {
        banner();
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    if (!command_exists("userdel")) {
        std::cerr << RED << "ERROR:" << RESET << " missing userdel.\n";
        return 1;
    }

    Terminal terminal;
    Options options;
    std::vector<UserEntry> users = read_users(options.showSystemUsers);
    int cursor = 0;
    int offset = 0;

    while (true) {
        const int maxRow = static_cast<int>(users.size()) + 4;
        if (cursor > maxRow) cursor = maxRow;

        constexpr int pageSize = 12;
        if (cursor < static_cast<int>(users.size())) {
            if (cursor < offset) offset = cursor;
            if (cursor >= offset + pageSize) offset = cursor - pageSize + 1;
        }

        draw(users, options, cursor, offset);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, maxRow);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, maxRow);
        } else if (key.key == Key::Enter) {
            const int removeHomeRow = static_cast<int>(users.size());
            const int forceRow = removeHomeRow + 1;
            const int showSystemRow = removeHomeRow + 2;
            const int deleteRow = removeHomeRow + 3;
            const int cancelRow = removeHomeRow + 4;

            options.message.clear();
            if (cursor < static_cast<int>(users.size())) {
                users[cursor].selected = !users[cursor].selected;
            } else if (cursor == removeHomeRow) {
                options.removeHome = !options.removeHome;
            } else if (cursor == forceRow) {
                options.force = !options.force;
            } else if (cursor == showSystemRow) {
                options.showSystemUsers = !options.showSystemUsers;
                users = read_users(options.showSystemUsers);
                cursor = 0;
                offset = 0;
            } else if (cursor == deleteRow) {
                if (selected_count(users) == 0) {
                    options.message = "Select at least one user.";
                } else if (confirm_delete(users, options)) {
                    return run_deletion(users, options);
                }
            } else if (cursor == cancelRow) {
                return 0;
            }
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
    }

    return run_tui();
}

