#include "main.h"

#include <cerrno>
#include <cstring>
#include <grp.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class Key {
    Up,
    Down,
    Tab,
    Enter,
    Backspace,
    Escape,
    CtrlC,
    Character,
    Unknown
};

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

struct GroupEntry {
    std::string name;
    gid_t gid = 0;
    std::string members;
    bool selected = false;
};

struct Options {
    bool force = false;
    bool showSystemGroups = false;
    std::string keyword;
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
    std::cout << "              kgroupdel\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kgroupdel component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kgroupdel [options]\n";
    std::cout << "Interactive terminal GUI for deleting groups.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Tab           Jump to settings/Top\n";
    std::cout << "  Enter         Select group, edit keyword, toggle option, or run action\n";
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
        return {Key::Unknown, '\0'};
    }
    if (c >= 32 && c <= 126) return {Key::Character, c};
    return {Key::Unknown, '\0'};
}

std::string yes_no(bool value) {
    return value ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
}

std::string join_members(char** members) {
    std::string result;
    if (members == nullptr) return result;
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

int selected_count(const std::vector<GroupEntry>& groups) {
    return static_cast<int>(std::count_if(groups.begin(), groups.end(), [](const GroupEntry& group) {
        return group.selected;
    }));
}

void draw_row(int row, int cursor, const std::string& text, bool danger = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (danger) std::cout << RED;
    std::cout << text << RESET << '\n';
}

void draw(const std::vector<GroupEntry>& groups, const Options& options, int cursor, int offset) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " settings  "
              << CYAN << "Enter:" << RESET << " select/toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n";
    std::cout << "Selected groups: " << GREEN << selected_count(groups) << RESET << "\n\n";

    constexpr int pageSize = 12;
    const int end = std::min<int>(static_cast<int>(groups.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        const auto& group = groups[i];
        std::ostringstream row;
        row << (group.selected ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
        row << group.name << "  gid:" << group.gid;
        if (!group.members.empty()) row << "  members:" << group.members;
        draw_row(i, cursor, row.str());
    }

    if (groups.empty()) std::cout << "  No groups to show.\n";

    const int forceRow = static_cast<int>(groups.size());
    const int showSystemRow = forceRow + 1;
    const int keywordRow = forceRow + 2;
    const int selectKeywordRow = forceRow + 3;
    const int deleteRow = forceRow + 4;
    const int cancelRow = forceRow + 5;

    std::cout << '\n';
    draw_row(forceRow, cursor, "Force delete        " + yes_no(options.force));
    draw_row(showSystemRow, cursor, "Show system groups  " + yes_no(options.showSystemGroups));
    draw_row(keywordRow, cursor, "Keyword             " + (options.keyword.empty() ? DIM + "(empty)" + RESET : options.keyword));
    draw_row(selectKeywordRow, cursor, "Select by keyword   Enter");
    draw_row(deleteRow, cursor, "Delete selected     Enter", true);
    draw_row(cancelRow, cursor, "Cancel              Enter or q");

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
    return run_process({"sh", "-c", "command -v " + command + " >/dev/null 2>&1"}).exitCode == 0;
}

std::vector<GroupEntry> selected_groups(const std::vector<GroupEntry>& groups) {
    std::vector<GroupEntry> out;
    for (const auto& group : groups) {
        if (group.selected) out.push_back(group);
    }
    return out;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lower_text(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string edit_keyword(std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " Delete with keyword\n";
        std::cout << "Groups containing this text will be selected. Enter saves, Esc cancels.\n\n";
        std::cout << BLUE << "Keyword" << RESET << ": " << value << std::flush;

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

int select_by_keyword(std::vector<GroupEntry>& groups, const std::string& keyword) {
    const std::string needle = lower_text(trim(keyword));
    if (needle.empty()) return 0;

    int matches = 0;
    for (auto& group : groups) {
        const std::string name = lower_text(group.name);
        if (name.find(needle) != std::string::npos) {
            group.selected = true;
            ++matches;
        }
    }
    return matches;
}

bool confirm_delete(const std::vector<GroupEntry>& groups, const Options& options) {
    clear_screen();
    banner();
    std::cout << RED << "Ready to delete groups" << RESET << "\n\n";
    for (const auto& group : selected_groups(groups)) {
        std::cout << "  - " << group.name << " (gid:" << group.gid << ")";
        if (!group.members.empty()) std::cout << " members:" << group.members;
        std::cout << '\n';
    }
    std::cout << "\nForce delete: " << (options.force ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to delete, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int delete_group(const GroupEntry& group, const Options& options) {
    std::vector<std::string> args = {"groupdel"};
    if (options.force) args.push_back("-f");
    args.push_back(group.name);
    return run_process(args).exitCode;
}

int run_deletion(const std::vector<GroupEntry>& groups, const Options& options) {
    clear_screen();
    banner();

    int failures = 0;
    for (const auto& group : selected_groups(groups)) {
        std::cout << CYAN << "[*]" << RESET << " Deleting group " << group.name << "...\n";
        const int result = delete_group(group, options);
        if (result == 0) {
            std::cout << GREEN << "[+]" << RESET << " Deleted " << group.name << '\n';
        } else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " Failed " << group.name << " (exit " << result << ")\n";
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

    if (!command_exists("groupdel")) {
        std::cerr << RED << "ERROR:" << RESET << " missing groupdel.\n";
        return 1;
    }

    Terminal terminal;
    Options options;
    std::vector<GroupEntry> groups = read_groups(options.showSystemGroups);
    int cursor = 0;
    int offset = 0;

    while (true) {
        const int maxRow = static_cast<int>(groups.size()) + 5;
        if (cursor > maxRow) cursor = maxRow;

        constexpr int pageSize = 12;
        if (cursor < static_cast<int>(groups.size())) {
            if (cursor < offset) offset = cursor;
            if (cursor >= offset + pageSize) offset = cursor - pageSize + 1;
        }

        draw(groups, options, cursor, offset);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, maxRow);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, maxRow);
        } else if (key.key == Key::Tab) {
            cursor = cursor < static_cast<int>(groups.size()) ? static_cast<int>(groups.size()) : 0;
            offset = 0;
        } else if (key.key == Key::Enter) {
            const int forceRow = static_cast<int>(groups.size());
            const int showSystemRow = forceRow + 1;
            const int keywordRow = forceRow + 2;
            const int selectKeywordRow = forceRow + 3;
            const int deleteRow = forceRow + 4;
            const int cancelRow = forceRow + 5;

            options.message.clear();
            if (cursor < static_cast<int>(groups.size())) {
                groups[cursor].selected = !groups[cursor].selected;
            } else if (cursor == forceRow) {
                options.force = !options.force;
            } else if (cursor == showSystemRow) {
                options.showSystemGroups = !options.showSystemGroups;
                groups = read_groups(options.showSystemGroups);
                cursor = 0;
                offset = 0;
            } else if (cursor == keywordRow) {
                options.keyword = edit_keyword(options.keyword);
            } else if (cursor == selectKeywordRow) {
                const int matches = select_by_keyword(groups, options.keyword);
                if (matches == 0) {
                    options.message = "No groups matched keyword.";
                } else {
                    options.message = "Selected groups matching keyword: " + std::to_string(matches);
                }
            } else if (cursor == deleteRow) {
                if (selected_count(groups) == 0) {
                    options.message = "Select at least one group.";
                } else if (confirm_delete(groups, options)) {
                    return run_deletion(groups, options);
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
        if (arg == "--panel") continue;
    }

    return run_tui();
}
