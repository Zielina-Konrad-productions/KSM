#include "main.h"

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class Key { Up, Down, Left, Right, Tab, Enter, Backspace, Escape, CtrlC, Character, Unknown };

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

struct UserEntry {
    std::string name;
    uid_t uid = 0;
    std::string fullName;
    std::string home;
    std::string shell;
};

struct Options {
    std::string selectedUser;
    std::string newLogin;
    std::string fullName;
    std::string originalFullName;
    std::string homePath;
    std::string originalHomePath;
    bool moveHome = false;
    std::string shell;
    std::string originalShell;
    std::string groups;
    bool appendGroups = true;
    bool lockPassword = false;
    bool unlockPassword = false;
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
        if (rawEnabled_) tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
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
    std::cout << "              kusermod\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kusermod component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kusermod [options]\n";
    std::cout << "Interactive terminal GUI for modifying users.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Tab           Jump to settings/Top\n";
    std::cout << "  Enter         Select user, edit field, choose groups, toggle, or apply\n";
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
    if (c == '\t') return {Key::Tab, '\0'};
    if (c == '\n' || c == '\r') return {Key::Enter, '\0'};
    if (c == 127 || c == 8) return {Key::Backspace, '\0'};
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

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string yes_no(bool value) {
    return value ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
}

std::string field_value(const std::string& value) {
    return value.empty() ? DIM + "(unchanged)" + RESET : value;
}

std::vector<std::string> split_groups(const std::string& groups) {
    std::vector<std::string> result;
    std::stringstream ss(groups);
    std::string item;

    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) result.push_back(item);
    }

    return result;
}

std::string join_groups(const std::vector<std::string>& groups) {
    std::string result;
    for (const auto& group : groups) {
        if (!result.empty()) result += ",";
        result += group;
    }
    return result;
}

std::vector<std::string> system_groups() {
    std::vector<std::string> groups;
    std::ifstream file("/etc/group");
    std::string line;

    while (std::getline(file, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;

        const std::string name = trim(line.substr(0, pos));
        if (!name.empty()) groups.push_back(name);
    }

    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    return groups;
}

bool contains_group(const std::vector<std::string>& groups, const std::string& group) {
    return std::find(groups.begin(), groups.end(), group) != groups.end();
}

void toggle_group(std::vector<std::string>& groups, const std::string& group) {
    const auto it = std::find(groups.begin(), groups.end(), group);
    if (it == groups.end()) {
        groups.push_back(group);
        std::sort(groups.begin(), groups.end());
    } else {
        groups.erase(it);
    }
}

std::vector<UserEntry> read_users(bool showSystemUsers) {
    std::vector<UserEntry> users;
    setpwent();
    while (struct passwd* pw = getpwent()) {
        UserEntry user;
        user.name = pw->pw_name ? pw->pw_name : "";
        user.uid = pw->pw_uid;
        user.fullName = pw->pw_gecos ? pw->pw_gecos : "";
        user.home = pw->pw_dir ? pw->pw_dir : "";
        user.shell = pw->pw_shell ? pw->pw_shell : "";
        if (user.name.empty() || user.name == "root") continue;
        if (!showSystemUsers && (user.uid < 1000 || user.name == "nobody")) continue;
        users.push_back(user);
    }
    endpwent();

    std::sort(users.begin(), users.end(), [](const UserEntry& a, const UserEntry& b) {
        return a.name < b.name;
    });
    return users;
}

void load_user_into_options(const UserEntry& user, Options& options) {
    options.selectedUser = user.name;
    options.newLogin.clear();
    options.fullName = user.fullName;
    options.originalFullName = user.fullName;
    options.homePath = user.home;
    options.originalHomePath = user.home;
    options.shell = user.shell;
    options.originalShell = user.shell;
    options.message = "Loaded " + user.name + ".";
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

void draw_row(int row, int cursor, const std::string& text, bool action = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (action) std::cout << GREEN;
    std::cout << text << RESET << '\n';
}

void draw_field(int row, int cursor, const std::string& label, const std::string& value) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    std::cout << label;
    if (label.size() < 22) std::cout << std::string(22 - label.size(), ' ');
    std::cout << value << RESET << '\n';
}

void draw(const std::vector<UserEntry>& users, const Options& options, int cursor, int offset) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " settings  "
              << CYAN << "Enter:" << RESET << " select/edit/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    constexpr int pageSize = 9;
    const int end = std::min<int>(static_cast<int>(users.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        const auto& user = users[i];
        std::ostringstream row;
        row << (options.selectedUser == user.name ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
        row << user.name << "  uid:" << user.uid << "  shell:" << (user.shell.empty() ? "-" : user.shell);
        draw_row(i, cursor, row.str());
    }
    if (users.empty()) std::cout << "  No users to show.\n";

    const int base = static_cast<int>(users.size());
    std::cout << '\n' << BOLD << "Modify user: " << RESET
              << (options.selectedUser.empty() ? DIM + "(none)" + RESET : CYAN + options.selectedUser + RESET) << '\n';
    draw_field(base, cursor, "New login", field_value(options.newLogin));
    draw_field(base + 1, cursor, "Full name", field_value(options.fullName));
    draw_field(base + 2, cursor, "Home path", field_value(options.homePath));
    draw_row(base + 3, cursor, "Move home             " + yes_no(options.moveHome));
    draw_field(base + 4, cursor, "Login shell", field_value(options.shell));
    draw_field(base + 5, cursor, "Groups", field_value(options.groups));
    draw_row(base + 6, cursor, "Append groups         " + yes_no(options.appendGroups));
    draw_row(base + 7, cursor, "Lock password         " + yes_no(options.lockPassword));
    draw_row(base + 8, cursor, "Unlock password       " + yes_no(options.unlockPassword));
    draw_row(base + 9, cursor, "Show system users     " + yes_no(options.showSystemUsers));
    draw_row(base + 10, cursor, "Apply changes         Enter", true);
    draw_row(base + 11, cursor, "Cancel                Enter or q");

    if (!options.message.empty()) std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    std::cout << std::flush;
}

std::string edit_value(const std::string& title, std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " " << title << '\n';
        std::cout << "Enter saves, Esc cancels, Backspace deletes.\n\n";
        std::cout << BLUE << title << RESET << ": " << value << std::flush;
        const KeyPress key = read_key();
        if (key.key == Key::Enter) return trim(value);
        if (key.key == Key::Escape || key.key == Key::CtrlC) return value;
        if (key.key == Key::Backspace) {
            if (!value.empty()) value.pop_back();
        } else if (key.key == Key::Character) {
            value.push_back(key.value);
        }
    }
}

std::string select_groups(std::string current) {
    const std::vector<std::string> groups = system_groups();
    if (groups.empty()) {
        return current;
    }

    std::vector<std::string> selected = split_groups(current);
    int cursor = 0;
    int offset = 0;
    constexpr int pageSize = 14;

    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Group selector" << RESET << '\n';
        std::cout << "Up/Down move, Enter toggles, Esc/q saves.\n\n";

        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize) offset = cursor - pageSize + 1;

        const int end = std::min<int>(static_cast<int>(groups.size()), offset + pageSize);
        for (int i = offset; i < end; ++i) {
            const bool active = i == cursor;
            const bool checked = contains_group(selected, groups[i]);

            std::cout << (active ? BLUE : "");
            std::cout << (active ? "> " : "  ");
            std::cout << (checked ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET)
                      << groups[i] << RESET << '\n';
        }

        std::cout << "\nSelected: " << (selected.empty() ? "(none)" : join_groups(selected)) << '\n';
        std::cout << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Up) {
            cursor = std::max(0, cursor - 1);
        } else if (key.key == Key::Down) {
            cursor = std::min<int>(static_cast<int>(groups.size()) - 1, cursor + 1);
        } else if (key.key == Key::Enter) {
            toggle_group(selected, groups[cursor]);
        } else if (key.key == Key::Escape || key.key == Key::CtrlC ||
                   (key.key == Key::Character && key.value == 'q')) {
            return join_groups(selected);
        }
    }
}

bool valid_username(const std::string& name) {
    if (name.empty()) return true;
    if (!std::islower(static_cast<unsigned char>(name[0])) &&
        !std::isdigit(static_cast<unsigned char>(name[0])) &&
        name[0] != '_') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-';
    });
}

bool has_changes(const Options& options) {
    return !options.newLogin.empty() ||
           options.fullName != options.originalFullName ||
           options.homePath != options.originalHomePath ||
           options.shell != options.originalShell ||
           !options.groups.empty() ||
           options.lockPassword ||
           options.unlockPassword;
}

bool validate_options(Options& options) {
    if (options.selectedUser.empty()) {
        options.message = "Select user first.";
        return false;
    }
    if (!valid_username(options.newLogin)) {
        options.message = "Invalid new login.";
        return false;
    }
    if (options.lockPassword && options.unlockPassword) {
        options.message = "Choose lock or unlock, not both.";
        return false;
    }
    if (!has_changes(options)) {
        options.message = "Nothing to apply.";
        return false;
    }
    options.message.clear();
    return true;
}

bool confirm_apply(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to modify user" << RESET << "\n\n";
    std::cout << "User        : " << options.selectedUser << '\n';
    std::cout << "New login   : " << (options.newLogin.empty() ? "-" : options.newLogin) << '\n';
    std::cout << "Full name   : " << (options.fullName.empty() ? "-" : options.fullName) << '\n';
    std::cout << "Home path   : " << (options.homePath.empty() ? "-" : options.homePath) << '\n';
    std::cout << "Shell       : " << (options.shell.empty() ? "-" : options.shell) << '\n';
    std::cout << "Groups      : " << (options.groups.empty() ? "-" : options.groups) << '\n';
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to apply, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int apply_changes(const Options& options) {
    clear_screen();
    banner();

    std::vector<std::string> args = {"usermod"};
    bool usermodChange = false;
    if (!options.newLogin.empty()) {
        args.push_back("-l");
        args.push_back(options.newLogin);
        usermodChange = true;
    }
    if (options.fullName != options.originalFullName) {
        args.push_back("-c");
        args.push_back(options.fullName);
        usermodChange = true;
    }
    if (options.homePath != options.originalHomePath) {
        if (options.moveHome) args.push_back("-m");
        args.push_back("-d");
        args.push_back(options.homePath);
        usermodChange = true;
    }
    if (options.shell != options.originalShell) {
        args.push_back("-s");
        args.push_back(options.shell);
        usermodChange = true;
    }
    if (!options.groups.empty()) {
        if (options.appendGroups) args.push_back("-a");
        args.push_back("-G");
        args.push_back(options.groups);
        usermodChange = true;
    }
    args.push_back(options.selectedUser);

    int failures = 0;
    int code = 0;
    if (usermodChange) {
        std::cout << CYAN << "[*]" << RESET << " Running usermod...\n";
        code = run_process(args).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " User modified.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " usermod failed (exit " << code << ")\n";
        }
    }

    const std::string finalLogin = options.newLogin.empty() ? options.selectedUser : options.newLogin;
    if (options.lockPassword) {
        code = run_process({"passwd", "-l", finalLogin}).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Password locked.\n";
        else ++failures;
    }
    if (options.unlockPassword) {
        code = run_process({"passwd", "-u", finalLogin}).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Password unlocked.\n";
        else ++failures;
    }

    std::cout << '\n' << (failures == 0 ? GREEN + "Done." : RED + "Done with failures: " + std::to_string(failures))
              << RESET << '\n';
    std::cout << "Press any key to exit.\n";
    read_key();
    return failures == 0 ? 0 : 1;
}

void move_cursor(int& cursor, int delta, int maxRow) {
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

    auto users = read_users(options.showSystemUsers);
    Terminal terminal;
    int cursor = 0;
    int offset = 0;
    int exitCode = 0;

    while (true) {
        const int maxRow = static_cast<int>(users.size()) + 11;
        draw(users, options, cursor, offset);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1, maxRow);
        else if (key.key == Key::Down) move_cursor(cursor, 1, maxRow);
        else if (key.key == Key::Tab) cursor = cursor < static_cast<int>(users.size()) ? static_cast<int>(users.size()) : 0;
        else if (key.key == Key::Left || key.key == Key::Right) {
            const int base = static_cast<int>(users.size());
            if (cursor == base + 3) options.moveHome = !options.moveHome;
            if (cursor == base + 6) options.appendGroups = !options.appendGroups;
            if (cursor == base + 7) options.lockPassword = !options.lockPassword;
            if (cursor == base + 8) options.unlockPassword = !options.unlockPassword;
            if (cursor == base + 9) {
                options.showSystemUsers = !options.showSystemUsers;
                users = read_users(options.showSystemUsers);
                cursor = 0;
                offset = 0;
            }
        } else if (key.key == Key::Enter) {
            const int base = static_cast<int>(users.size());
            options.message.clear();
            if (cursor >= 0 && cursor < static_cast<int>(users.size())) {
                load_user_into_options(users[cursor], options);
            } else if (cursor == base) options.newLogin = edit_value("New login", options.newLogin);
            else if (cursor == base + 1) options.fullName = edit_value("Full name", options.fullName);
            else if (cursor == base + 2) options.homePath = edit_value("Home path", options.homePath);
            else if (cursor == base + 3) options.moveHome = !options.moveHome;
            else if (cursor == base + 4) options.shell = edit_value("Login shell", options.shell);
            else if (cursor == base + 5) options.groups = select_groups(options.groups);
            else if (cursor == base + 6) options.appendGroups = !options.appendGroups;
            else if (cursor == base + 7) options.lockPassword = !options.lockPassword;
            else if (cursor == base + 8) options.unlockPassword = !options.unlockPassword;
            else if (cursor == base + 9) {
                options.showSystemUsers = !options.showSystemUsers;
                users = read_users(options.showSystemUsers);
                cursor = 0;
                offset = 0;
            } else if (cursor == base + 10) {
                if (validate_options(options) && confirm_apply(options)) {
                    exitCode = apply_changes(options);
                    return exitCode;
                }
            } else if (cursor == base + 11) return 0;
        }

        constexpr int pageSize = 9;
        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize && cursor < static_cast<int>(users.size())) offset = cursor - pageSize + 1;
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
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kusermod --help' to list options.\n";
        return 1;
    }
    return run_tui(options);
}
