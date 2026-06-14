#include "main.h"

#include <cerrno>
#include <cstring>
#include <grp.h>
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

struct GroupEntry {
    std::string name;
    gid_t gid = 0;
    std::string members;
};

struct Options {
    std::string selectedGroup;
    std::string newName;
    std::string gid;
    std::string originalGid;
    std::string members;
    bool replaceMembers = false;
    bool showSystemGroups = false;
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
    std::cout << "              kgroupmod\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kgroupmod component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kgroupmod [options]\n";
    std::cout << "Interactive terminal GUI for modifying groups.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Tab           Jump to settings/Top\n";
    std::cout << "  Enter         Select group, edit field, toggle, or apply\n";
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

std::string join_members(char** members) {
    std::string result;
    if (!members) return result;
    for (int i = 0; members[i] != nullptr; ++i) {
        if (!result.empty()) result += ",";
        result += members[i];
    }
    return result;
}

bool is_normal_group(const GroupEntry& group) {
    return group.gid >= 1000 && group.name != "nogroup" && group.name != "nobody";
}

std::vector<GroupEntry> read_groups(bool showSystemGroups) {
    std::vector<GroupEntry> groups;
    setgrent();
    while (struct group* gr = getgrent()) {
        GroupEntry group;
        group.name = gr->gr_name ? gr->gr_name : "";
        group.gid = gr->gr_gid;
        group.members = join_members(gr->gr_mem);
        if (group.name.empty() || group.name == "root") continue;
        if (!showSystemGroups && !is_normal_group(group)) continue;
        groups.push_back(group);
    }
    endgrent();
    std::sort(groups.begin(), groups.end(), [](const GroupEntry& a, const GroupEntry& b) {
        return a.name < b.name;
    });
    return groups;
}

void load_group_into_options(const GroupEntry& group, Options& options) {
    options.selectedGroup = group.name;
    options.newName.clear();
    options.gid = std::to_string(group.gid);
    options.originalGid = options.gid;
    options.members = group.members;
    options.message = "Loaded " + group.name + ".";
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
    if (label.size() < 20) std::cout << std::string(20 - label.size(), ' ');
    std::cout << value << RESET << '\n';
}

void draw(const std::vector<GroupEntry>& groups, const Options& options, int cursor, int offset) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " settings  "
              << CYAN << "Enter:" << RESET << " select/edit/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    constexpr int pageSize = 10;
    const int end = std::min<int>(static_cast<int>(groups.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        const auto& group = groups[i];
        std::ostringstream row;
        row << (options.selectedGroup == group.name ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
        row << group.name << "  gid:" << group.gid;
        if (!group.members.empty()) row << "  members:" << group.members;
        draw_row(i, cursor, row.str());
    }
    if (groups.empty()) std::cout << "  No groups to show.\n";

    const int base = static_cast<int>(groups.size());
    std::cout << '\n' << BOLD << "Modify group: " << RESET
              << (options.selectedGroup.empty() ? DIM + "(none)" + RESET : CYAN + options.selectedGroup + RESET) << '\n';
    draw_field(base, cursor, "New name", field_value(options.newName));
    draw_field(base + 1, cursor, "GID", field_value(options.gid));
    draw_field(base + 2, cursor, "Members CSV", field_value(options.members));
    draw_row(base + 3, cursor, "Replace members      " + yes_no(options.replaceMembers));
    draw_row(base + 4, cursor, "Show system groups   " + yes_no(options.showSystemGroups));
    draw_row(base + 5, cursor, "Apply changes        Enter", true);
    draw_row(base + 6, cursor, "Cancel               Enter or q");

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

bool valid_group_name(const std::string& name) {
    if (name.empty()) return true;
    if (!std::islower(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-';
    });
}

bool valid_gid(const std::string& gid) {
    if (gid.empty()) return true;
    return std::all_of(gid.begin(), gid.end(), [](unsigned char ch) {
        return std::isdigit(ch);
    });
}

bool has_changes(const Options& options) {
    return !options.newName.empty() ||
           (!options.gid.empty() && options.gid != options.originalGid) ||
           options.replaceMembers;
}

bool validate_options(Options& options) {
    if (options.selectedGroup.empty()) {
        options.message = "Select group first.";
        return false;
    }
    if (!valid_group_name(options.newName)) {
        options.message = "Invalid new group name.";
        return false;
    }
    if (!valid_gid(options.gid)) {
        options.message = "Invalid GID.";
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
    std::cout << YELLOW << "Ready to modify group" << RESET << "\n\n";
    std::cout << "Group       : " << options.selectedGroup << '\n';
    std::cout << "New name    : " << (options.newName.empty() ? "-" : options.newName) << '\n';
    std::cout << "GID         : " << (options.gid.empty() ? "-" : options.gid) << '\n';
    std::cout << "Members CSV : " << (options.members.empty() ? "-" : options.members) << '\n';
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to apply, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int apply_changes(const Options& options) {
    clear_screen();
    banner();

    std::vector<std::string> args = {"groupmod"};
    bool groupmodChange = false;
    if (!options.newName.empty()) {
        args.push_back("-n");
        args.push_back(options.newName);
        groupmodChange = true;
    }
    if (!options.gid.empty() && options.gid != options.originalGid) {
        args.push_back("-g");
        args.push_back(options.gid);
        groupmodChange = true;
    }
    args.push_back(options.selectedGroup);

    int failures = 0;
    int code = 0;
    if (groupmodChange) {
        std::cout << CYAN << "[*]" << RESET << " Running groupmod...\n";
        code = run_process(args).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Group modified.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " groupmod failed (exit " << code << ")\n";
        }
    }

    const std::string finalGroup = options.newName.empty() ? options.selectedGroup : options.newName;
    if (options.replaceMembers) {
        code = run_process({"gpasswd", "-M", options.members, finalGroup}).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Members replaced.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " member update failed (exit " << code << ")\n";
        }
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

    auto groups = read_groups(options.showSystemGroups);
    Terminal terminal;
    int cursor = 0;
    int offset = 0;

    while (true) {
        const int maxRow = static_cast<int>(groups.size()) + 6;
        draw(groups, options, cursor, offset);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1, maxRow);
        else if (key.key == Key::Down) move_cursor(cursor, 1, maxRow);
        else if (key.key == Key::Tab) cursor = cursor < static_cast<int>(groups.size()) ? static_cast<int>(groups.size()) : 0;
        else if (key.key == Key::Left || key.key == Key::Right) {
            const int base = static_cast<int>(groups.size());
            if (cursor == base + 3) options.replaceMembers = !options.replaceMembers;
            if (cursor == base + 4) {
                options.showSystemGroups = !options.showSystemGroups;
                groups = read_groups(options.showSystemGroups);
                cursor = 0;
                offset = 0;
            }
        } else if (key.key == Key::Enter) {
            const int base = static_cast<int>(groups.size());
            options.message.clear();
            if (cursor >= 0 && cursor < static_cast<int>(groups.size())) load_group_into_options(groups[cursor], options);
            else if (cursor == base) options.newName = edit_value("New name", options.newName);
            else if (cursor == base + 1) options.gid = edit_value("GID", options.gid);
            else if (cursor == base + 2) options.members = edit_value("Members CSV", options.members);
            else if (cursor == base + 3) options.replaceMembers = !options.replaceMembers;
            else if (cursor == base + 4) {
                options.showSystemGroups = !options.showSystemGroups;
                groups = read_groups(options.showSystemGroups);
                cursor = 0;
                offset = 0;
            } else if (cursor == base + 5) {
                if (validate_options(options) && confirm_apply(options)) return apply_changes(options);
            } else if (cursor == base + 6) return 0;
        }

        constexpr int pageSize = 10;
        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize && cursor < static_cast<int>(groups.size())) offset = cursor - pageSize + 1;
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
        if (arg == "--panel") continue;
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kgroupmod --help' to list options.\n";
        return 1;
    }
    return run_tui(options);
}
