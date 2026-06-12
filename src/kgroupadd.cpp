#include "main.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class Key {
    Up,
    Down,
    Left,
    Right,
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

struct UserEntry {
    std::string name;
    uid_t uid = 0;
    bool selected = false;
};

struct Options {
    bool many = false;
    int count = 1;
    std::string groupTemplate;
    std::string gidStart;
    bool systemGroup = false;
    std::string members;
    std::string message;
};

struct GroupPlan {
    int index = 1;
    std::string name;
    std::string gid;
    std::string members;
    bool systemGroup = false;
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
    std::cout << "              kgroupadd\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kgroupadd component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kgroupadd [options]\n";
    std::cout << "Interactive terminal GUI for creating groups.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move between fields\n";
    std::cout << "  Left/Right    Change checkboxes and counters\n";
    std::cout << "  Enter         Edit text, toggle, select, or run action\n";
    std::cout << "  q             Cancel\n\n";
    std::cout << BLUE << "Template value:" << RESET << '\n';
    std::cout << "  Use $i for multi-group generation. team$i becomes team1, team2, team3.\n";
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

std::string replace_index(std::string value, int index) {
    const std::string marker = "$i";
    const std::string replacement = std::to_string(index);
    size_t pos = 0;
    while ((pos = value.find(marker, pos)) != std::string::npos) {
        value.replace(pos, marker.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ",";
        result += value;
    }
    return result;
}

bool contains_value(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<UserEntry> read_users() {
    std::vector<UserEntry> users;
    setpwent();
    while (struct passwd* pw = getpwent()) {
        UserEntry user;
        user.name = pw->pw_name ? pw->pw_name : "";
        user.uid = pw->pw_uid;
        if (user.name.empty() || user.name == "root" || user.name == "nobody") continue;
        if (user.uid < 1000) continue;
        users.push_back(user);
    }
    endpwent();

    std::sort(users.begin(), users.end(), [](const UserEntry& a, const UserEntry& b) {
        return a.name < b.name;
    });
    return users;
}

bool group_exists(const std::string& name) {
    return getgrnam(name.c_str()) != nullptr;
}

bool gid_exists(const std::string& gidText) {
    if (gidText.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(gidText.c_str(), &end, 10);
    if (errno != 0 || end == gidText.c_str() || *end != '\0') return false;
    return getgrgid(static_cast<gid_t>(parsed)) != nullptr;
}

bool digits_only(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch);
    });
}

int effective_count(const Options& options) {
    return options.many ? options.count : 1;
}

std::vector<GroupPlan> build_plan(const Options& options) {
    std::vector<GroupPlan> plan;
    const bool hasGid = !trim(options.gidStart).empty();
    const unsigned long gidBase = hasGid ? std::strtoul(options.gidStart.c_str(), nullptr, 10) : 0;

    for (int i = 1; i <= effective_count(options); ++i) {
        GroupPlan group;
        group.index = i;
        group.name = replace_index(options.groupTemplate, i);
        group.members = options.members;
        group.systemGroup = options.systemGroup;
        if (hasGid) {
            group.gid = std::to_string(gidBase + static_cast<unsigned long>(i - 1));
        }
        plan.push_back(group);
    }
    return plan;
}

void draw_field(int index, int selected, const std::string& label, const std::string& value, bool disabled = false) {
    const bool active = (index == selected);
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (disabled) std::cout << DIM;
    std::cout << label;
    if (label.size() < 22) {
        std::cout << std::string(22 - label.size(), ' ');
    }
    std::cout << value << RESET << '\n';
}

void draw_preview(const Options& options) {
    const auto plan = build_plan(options);
    std::cout << '\n' << CYAN << "Preview" << RESET << '\n';
    std::cout << DIM << "----------------------------------------" << RESET << '\n';

    const int maxRows = std::min<int>(static_cast<int>(plan.size()), 5);
    for (int i = 0; i < maxRows; ++i) {
        const auto& group = plan[i];
        std::cout << "#" << group.index << " " << group.name
                  << " | gid:" << (group.gid.empty() ? "auto" : group.gid)
                  << " | " << (group.systemGroup ? "system" : "regular")
                  << " | members:" << split_csv(group.members).size() << '\n';
    }
    if (static_cast<int>(plan.size()) > maxRows) {
        std::cout << "... +" << (static_cast<int>(plan.size()) - maxRows) << " more\n";
    }
}

void draw(const Options& options, int selected) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move/change  "
              << CYAN << "Enter:" << RESET << " edit/select/action  "
              << CYAN << "q:" << RESET << " cancel\n";
    std::cout << "Use $i in group template for multi-group generation.\n\n";

    draw_field(0, selected, "More than one", yes_no(options.many));
    draw_field(1, selected, "Amount", std::to_string(options.count), !options.many);
    draw_field(2, selected, "Group template", options.groupTemplate.empty() ? DIM + "(empty)" + RESET : options.groupTemplate);
    draw_field(3, selected, "GID start", options.gidStart.empty() ? DIM + "(auto)" + RESET : options.gidStart);
    draw_field(4, selected, "System group", yes_no(options.systemGroup));
    draw_field(5, selected, "Members", options.members.empty() ? DIM + "(none)" + RESET : options.members);
    draw_field(6, selected, "Create groups", "Enter");
    draw_field(7, selected, "Cancel", "Enter or q");

    draw_preview(options);
    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
    std::cout << std::flush;
}

char normalize_group_char(char ch) {
    if (std::isupper(static_cast<unsigned char>(ch))) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ch;
}

bool group_template_char_allowed(char ch) {
    return std::islower(static_cast<unsigned char>(ch)) ||
           std::isdigit(static_cast<unsigned char>(ch)) ||
           ch == '_' ||
           ch == '-' ||
           ch == '$' ||
           ch == 'i';
}

std::string edit_group_template(std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " Group template\n";
        std::cout << "Allowed: a-z, 0-9, _, -, and $i. Uppercase becomes lowercase.\n";
        std::cout << "Enter saves, Esc cancels, Backspace deletes.\n\n";
        std::cout << BLUE << "Group template" << RESET << ": " << value << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Enter) return trim(value);
        if (key.key == Key::Escape || key.key == Key::CtrlC) return value;
        if (key.key == Key::Backspace) {
            if (!value.empty()) value.pop_back();
        } else if (key.key == Key::Character) {
            const char normalized = normalize_group_char(key.value);
            if (group_template_char_allowed(normalized)) value.push_back(normalized);
        }
    }
}

std::string edit_gid(std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " GID start\n";
        std::cout << "Digits only. Empty means automatic GID. Enter saves, Esc cancels.\n\n";
        std::cout << BLUE << "GID start" << RESET << ": " << value << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Enter) return trim(value);
        if (key.key == Key::Escape || key.key == Key::CtrlC) return value;
        if (key.key == Key::Backspace) {
            if (!value.empty()) value.pop_back();
        } else if (key.key == Key::Character && std::isdigit(static_cast<unsigned char>(key.value))) {
            value.push_back(key.value);
        }
    }
}

std::string select_members(std::string current) {
    std::vector<UserEntry> users = read_users();
    std::vector<std::string> selected = split_csv(current);
    int cursor = 0;
    int offset = 0;
    constexpr int pageSize = 14;

    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Member selector" << RESET << '\n';
        std::cout << "Up/Down move, Enter toggles, Esc/q saves.\n\n";

        if (users.empty()) {
            std::cout << "  No regular users to show.\n";
        }

        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize) offset = cursor - pageSize + 1;

        const int end = std::min<int>(static_cast<int>(users.size()), offset + pageSize);
        for (int i = offset; i < end; ++i) {
            const bool active = (i == cursor);
            const bool checked = contains_value(selected, users[i].name);
            std::cout << (active ? BLUE : "");
            std::cout << (active ? "> " : "  ");
            std::cout << (checked ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
            std::cout << users[i].name << " uid:" << users[i].uid << RESET << '\n';
        }

        std::cout << "\nSelected: " << (selected.empty() ? "(none)" : join_csv(selected)) << '\n';
        std::cout << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Up) {
            cursor = std::max(0, cursor - 1);
        } else if (key.key == Key::Down && !users.empty()) {
            cursor = std::min<int>(static_cast<int>(users.size()) - 1, cursor + 1);
        } else if (key.key == Key::Enter && !users.empty()) {
            const std::string& user = users[cursor].name;
            const auto it = std::find(selected.begin(), selected.end(), user);
            if (it == selected.end()) {
                selected.push_back(user);
                std::sort(selected.begin(), selected.end());
            } else {
                selected.erase(it);
            }
        } else if (key.key == Key::Escape || key.key == Key::CtrlC ||
                   (key.key == Key::Character && key.value == 'q')) {
            return join_csv(selected);
        }
    }
}

bool valid_group_name(const std::string& name) {
    if (name.empty()) return false;
    if (!std::islower(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-';
    });
}

bool validate_options(Options& options) {
    if (trim(options.groupTemplate).empty()) {
        options.message = "Group template cannot be empty.";
        return false;
    }
    if (!options.gidStart.empty() && !digits_only(options.gidStart)) {
        options.message = "GID start must contain digits only.";
        return false;
    }

    std::vector<std::string> plannedNames;
    for (const auto& group : build_plan(options)) {
        if (!valid_group_name(group.name)) {
            options.message = "Invalid group name after $i expansion: " + group.name;
            return false;
        }
        if (contains_value(plannedNames, group.name)) {
            options.message = "Duplicate planned group: " + group.name;
            return false;
        }
        plannedNames.push_back(group.name);
        if (group_exists(group.name)) {
            options.message = "Group already exists: " + group.name;
            return false;
        }
        if (!group.gid.empty() && gid_exists(group.gid)) {
            options.message = "GID already exists: " + group.gid;
            return false;
        }
    }

    options.message.clear();
    return true;
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

int create_group(const GroupPlan& group) {
    std::vector<std::string> args = {"groupadd"};
    if (group.systemGroup) args.push_back("-r");
    if (!group.gid.empty()) {
        args.push_back("-g");
        args.push_back(group.gid);
    }
    args.push_back(group.name);

    ProcessResult result = run_process(args);
    if (result.exitCode != 0) return result.exitCode;

    for (const auto& member : split_csv(group.members)) {
        result = run_process({"gpasswd", "-a", member, group.name});
        if (result.exitCode != 0) return result.exitCode;
    }
    return 0;
}

bool confirm_create(const Options& options) {
    clear_screen();
    banner();
    std::cout << CYAN << "Ready to create groups" << RESET << "\n\n";
    draw_preview(options);
    std::cout << "\nMembers: " << (options.members.empty() ? "(none)" : options.members) << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to create, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int run_creation(const Options& options) {
    clear_screen();
    banner();

    int failures = 0;
    for (const auto& group : build_plan(options)) {
        std::cout << CYAN << "[*]" << RESET << " Creating group " << group.name << "...\n";
        const int result = create_group(group);
        if (result == 0) {
            std::cout << GREEN << "[+]" << RESET << " Created " << group.name << '\n';
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

void move_selection(int& selected, int delta) {
    constexpr int maxField = 7;
    selected += delta;
    if (selected < 0) selected = maxField;
    if (selected > maxField) selected = 0;
}

void adjust_field(Options& options, int selected, int delta) {
    if (selected == 0) {
        options.many = !options.many;
        if (options.many && options.count == 1) options.count = 2;
    } else if (selected == 1 && options.many) {
        options.count = std::max(1, options.count + delta);
    } else if (selected == 4) {
        options.systemGroup = !options.systemGroup;
    }
}

bool edit_field(Options& options, int selected, int& exitCode) {
    options.message.clear();
    if (selected == 0 || selected == 4) {
        adjust_field(options, selected, 1);
        return false;
    }
    if (selected == 1) {
        if (options.many) options.count = std::max(1, options.count + 1);
        return false;
    }
    if (selected == 2) options.groupTemplate = edit_group_template(options.groupTemplate);
    if (selected == 3) options.gidStart = edit_gid(options.gidStart);
    if (selected == 5) options.members = select_members(options.members);
    if (selected == 6) {
        if (!validate_options(options)) return false;
        if (!options.members.empty() && !command_exists("gpasswd")) {
            options.message = "Missing gpasswd for adding members.";
            return false;
        }
        if (confirm_create(options)) {
            exitCode = run_creation(options);
            return true;
        }
    }
    if (selected == 7) {
        exitCode = 0;
        return true;
    }
    return false;
}

int run_tui() {
    if (geteuid() != 0) {
        banner();
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    if (!command_exists("groupadd")) {
        std::cerr << RED << "ERROR:" << RESET << " missing groupadd.\n";
        return 1;
    }

    Terminal terminal;
    Options options;
    int selected = 0;

    while (true) {
        draw(options, selected);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_selection(selected, -1);
        } else if (key.key == Key::Down) {
            move_selection(selected, 1);
        } else if (key.key == Key::Left) {
            adjust_field(options, selected, -1);
        } else if (key.key == Key::Right) {
            adjust_field(options, selected, 1);
        } else if (key.key == Key::Enter) {
            int exitCode = 0;
            if (edit_field(options, selected, exitCode)) return exitCode;
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
