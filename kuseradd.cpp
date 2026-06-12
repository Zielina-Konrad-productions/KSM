#include "main.h"

#include <cerrno>
#include <cstring>
#include <sstream>
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

struct Options {
    bool many = false;
    int count = 1;
    std::string usernameTemplate;
    std::string passwordTemplate;
    std::string fullNameTemplate;
    bool createHome = true;
    std::string shellTemplate = "/bin/bash";
    std::string homeTemplate;
    std::string groupsTemplate;
    bool addSudoGroup = false;
    std::string message;
};

struct UserPlan {
    int index = 1;
    std::string username;
    std::string password;
    std::string fullName;
    std::string shell;
    std::string groups;
    std::string home;
    bool createHome = true;
};

struct ProcessResult {
    int exitCode = 1;
    std::string error;
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
    std::cout << "              kuseradd\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kuseradd component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kuseradd [options]\n";
    std::cout << "Interactive terminal GUI for creating users.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move between fields\n";
    std::cout << "  Left/Right    Change checkboxes and counters\n";
    std::cout << "  Enter         Edit text, toggle, or run action\n";
    std::cout << "  q             Cancel\n\n";
    std::cout << BLUE << "Template value:" << RESET << '\n';
    std::cout << "  Use $i in fields. For 3 users, user$i becomes user1, user2, user3.\n";
}

bool read_byte_timeout(char& c, int milliseconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);

    timeval timeout {};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;

    const int ready = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return false;
    }

    return read(STDIN_FILENO, &c, 1) == 1;
}

KeyPress read_key() {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return {Key::Unknown, '\0'};
    }

    if (c == 3) {
        return {Key::CtrlC, '\0'};
    }
    if (c == '\n' || c == '\r') {
        return {Key::Enter, '\0'};
    }
    if (c == 127 || c == 8) {
        return {Key::Backspace, '\0'};
    }
    if (c == 27) {
        char second = '\0';
        char third = '\0';
        if (!read_byte_timeout(second, 50)) {
            return {Key::Escape, '\0'};
        }
        if (second != '[' || !read_byte_timeout(third, 50)) {
            return {Key::Escape, '\0'};
        }
        if (third == 'A') return {Key::Up, '\0'};
        if (third == 'B') return {Key::Down, '\0'};
        if (third == 'C') return {Key::Right, '\0'};
        if (third == 'D') return {Key::Left, '\0'};
        return {Key::Unknown, '\0'};
    }
    if (c >= 32 && c <= 126) {
        return {Key::Character, c};
    }
    return {Key::Unknown, '\0'};
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string mask(const std::string& value) {
    if (value.empty()) {
        return DIM + "(empty)" + RESET;
    }
    return std::string(value.size(), '*');
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

std::string append_group(std::string groups, const std::string& group) {
    groups = trim(groups);
    if (groups.empty()) {
        return group;
    }

    std::stringstream ss(groups);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (trim(item) == group) {
            return groups;
        }
    }
    return groups + "," + group;
}

std::vector<std::string> split_groups(const std::string& groups) {
    std::vector<std::string> result;
    std::stringstream ss(groups);
    std::string item;

    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }

    return result;
}

std::string join_groups(const std::vector<std::string>& groups) {
    std::string result;
    for (const auto& group : groups) {
        if (!result.empty()) {
            result += ",";
        }
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
        if (pos == std::string::npos) {
            continue;
        }

        const std::string name = trim(line.substr(0, pos));
        if (!name.empty()) {
            groups.push_back(name);
        }
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

int effective_count(const Options& options) {
    return options.many ? options.count : 1;
}

std::vector<UserPlan> build_plan(const Options& options) {
    std::vector<UserPlan> plan;

    for (int i = 1; i <= effective_count(options); ++i) {
        UserPlan user;
        user.index = i;
        user.username = replace_index(options.usernameTemplate, i);
        user.password = replace_index(options.passwordTemplate, i);
        user.fullName = replace_index(options.fullNameTemplate, i);
        user.shell = replace_index(options.shellTemplate, i);
        user.home = replace_index(options.homeTemplate, i);
        user.groups = replace_index(options.groupsTemplate, i);
        user.createHome = options.createHome;

        if (options.addSudoGroup) {
            user.groups = append_group(user.groups, "sudo");
        }

        plan.push_back(user);
    }

    return plan;
}

void draw_field(int index, int selected, const std::string& label, const std::string& value, bool disabled = false) {
    const bool active = (index == selected);
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (disabled) {
        std::cout << "\033[2m";
    }
    std::cout << label;
    if (label.size() < 24) {
        std::cout << std::string(24 - label.size(), ' ');
    }
    std::cout << value << RESET << '\n';
}

void draw_preview(const Options& options) {
    const auto plan = build_plan(options);
    std::cout << '\n' << CYAN << "Preview" << RESET << '\n';
    std::cout << DIM << "----------------------------------------" << RESET << '\n';

    const int maxRows = std::min<int>(static_cast<int>(plan.size()), 4);
    for (int i = 0; i < maxRows; ++i) {
        const auto& user = plan[i];
        std::cout << "#" << user.index << " ";
        std::cout << user.username << " | ";
        std::cout << (user.fullName.empty() ? "-" : user.fullName) << " | ";
        std::cout << (user.groups.empty() ? "groups:none" : "groups:" + user.groups) << '\n';
    }
    if (static_cast<int>(plan.size()) > maxRows) {
        std::cout << "... +" << (static_cast<int>(plan.size()) - maxRows) << " more\n";
    }
}

void draw(const Options& options, int selected) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move/change  "
              << CYAN << "Enter:" << RESET << " edit/toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n";
    std::cout << "Use $i in templates for multi-user generation.\n\n";

    draw_field(0, selected, "More than one", yes_no(options.many));
    draw_field(1, selected, "Amount", std::to_string(options.count), !options.many);
    draw_field(2, selected, "Username template", options.usernameTemplate.empty() ? DIM + "(empty)" + RESET : options.usernameTemplate);
    draw_field(3, selected, "Password template", mask(options.passwordTemplate));
    draw_field(4, selected, "Full name template", options.fullNameTemplate.empty() ? DIM + "(empty)" + RESET : options.fullNameTemplate);
    draw_field(5, selected, "Create home", yes_no(options.createHome));
    draw_field(6, selected, "Login shell", options.shellTemplate);
    draw_field(7, selected, "Home path", options.homeTemplate.empty() ? DIM + "(system default)" + RESET : options.homeTemplate);
    draw_field(8, selected, "Extra groups", options.groupsTemplate.empty() ? DIM + "(none)" + RESET : options.groupsTemplate);
    draw_field(9, selected, "Add sudo group", yes_no(options.addSudoGroup));
    draw_field(10, selected, "Create users", "Enter");
    draw_field(11, selected, "Cancel", "Enter or q");

    draw_preview(options);
    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
    std::cout << std::flush;
}

std::string edit_value(const std::string& label, std::string value, bool secret) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " " << label << "\n";
        std::cout << "Enter saves, Esc cancels, Backspace deletes.\n\n";
        std::cout << BLUE << label << RESET << ": ";
        std::cout << (secret ? mask(value) : value) << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Enter) {
            return trim(value);
        }
        if (key.key == Key::Escape || key.key == Key::CtrlC) {
            return value;
        }
        if (key.key == Key::Backspace) {
            if (!value.empty()) {
                value.pop_back();
            }
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

        if (cursor < offset) {
            offset = cursor;
        }
        if (cursor >= offset + pageSize) {
            offset = cursor - pageSize + 1;
        }

        const int end = std::min<int>(static_cast<int>(groups.size()), offset + pageSize);
        for (int i = offset; i < end; ++i) {
            const bool active = (i == cursor);
            const bool checked = contains_group(selected, groups[i]);

            std::cout << (active ? BLUE : "");
            std::cout << (active ? "> " : "  ");
            std::cout << (checked ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET) << groups[i] << RESET << '\n';
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

bool username_template_char_allowed(char ch) {
    return std::islower(static_cast<unsigned char>(ch)) ||
           std::isdigit(static_cast<unsigned char>(ch)) ||
           ch == '_' ||
           ch == '-' ||
           ch == '$' ||
           ch == 'i';
}

char normalize_username_template_char(char ch) {
    if (std::isupper(static_cast<unsigned char>(ch))) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ch;
}

std::string edit_username_template(std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " Username template\n";
        std::cout << "Allowed: a-z, 0-9, _, -, and $i. Uppercase becomes lowercase.\n";
        std::cout << "Enter saves, Esc cancels, Backspace deletes.\n\n";

        std::cout << BLUE << "Username template" << RESET << ": " << value << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Enter) {
            return trim(value);
        }
        if (key.key == Key::Escape || key.key == Key::CtrlC) {
            return value;
        }
        if (key.key == Key::Backspace) {
            if (!value.empty()) {
                value.pop_back();
            }
        } else if (key.key == Key::Character) {
            const char normalized = normalize_username_template_char(key.value);
            if (username_template_char_allowed(normalized)) {
                value.push_back(normalized);
            }
        }
    }
}

bool valid_username(const std::string& username) {
    if (username.empty()) {
        return false;
    }
    if (!std::islower(static_cast<unsigned char>(username[0])) && username[0] != '_') {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-';
    });
}

bool validate_options(Options& options) {
    if (trim(options.usernameTemplate).empty()) {
        options.message = "Username template cannot be empty.";
        return false;
    }
    if (options.passwordTemplate.empty()) {
        options.message = "Password template cannot be empty.";
        return false;
    }

    for (const auto& user : build_plan(options)) {
        if (!valid_username(user.username)) {
            options.message = "Invalid username after $i expansion: " + user.username;
            return false;
        }
    }

    options.message.clear();
    return true;
}

ProcessResult run_process(const std::vector<std::string>& args, const std::string& stdinData = "") {
    int pipeFd[2] = {-1, -1};
    if (!stdinData.empty() && pipe(pipeFd) != 0) {
        return {1, std::strerror(errno)};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return {1, std::strerror(errno)};
    }

    if (pid == 0) {
        if (!stdinData.empty()) {
            close(pipeFd[1]);
            dup2(pipeFd[0], STDIN_FILENO);
            close(pipeFd[0]);
        }

        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (!stdinData.empty()) {
        close(pipeFd[0]);
        const char* data = stdinData.c_str();
        size_t remaining = stdinData.size();
        while (remaining > 0) {
            const ssize_t written = write(pipeFd[1], data, remaining);
            if (written <= 0) {
                break;
            }
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        close(pipeFd[1]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return {1, std::strerror(errno)};
    }

    if (WIFEXITED(status)) {
        return {WEXITSTATUS(status), ""};
    }
    if (WIFSIGNALED(status)) {
        return {128 + WTERMSIG(status), ""};
    }
    return {1, ""};
}

bool command_exists(const std::string& command) {
    const ProcessResult result = run_process({"sh", "-c", "command -v " + command + " >/dev/null 2>&1"});
    return result.exitCode == 0;
}

int create_user(const UserPlan& user) {
    std::vector<std::string> args = {"useradd"};
    if (user.createHome) {
        args.push_back("-m");
    }
    if (!user.fullName.empty()) {
        args.push_back("-c");
        args.push_back(user.fullName);
    }
    if (!user.shell.empty()) {
        args.push_back("-s");
        args.push_back(user.shell);
    }
    if (!user.home.empty()) {
        args.push_back("-d");
        args.push_back(user.home);
    }
    if (!user.groups.empty()) {
        args.push_back("-G");
        args.push_back(user.groups);
    }
    args.push_back(user.username);

    ProcessResult result = run_process(args);
    if (result.exitCode != 0) {
        return result.exitCode;
    }

    result = run_process({"chpasswd"}, user.username + ":" + user.password + "\n");
    return result.exitCode;
}

bool confirm_create(const Options& options) {
    clear_screen();
    banner();
    std::cout << CYAN << "Ready to create users" << RESET << "\n\n";
    draw_preview(options);
    std::cout << "\nPress " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to create, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int run_creation(const Options& options) {
    clear_screen();
    banner();

    int failures = 0;
    for (const auto& user : build_plan(options)) {
        std::cout << CYAN << "[*]" << RESET << " Creating " << user.username << "...\n";
        const int result = create_user(user);
        if (result == 0) {
            std::cout << GREEN << "[+]" << RESET << " Created " << user.username << '\n';
        } else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " Failed " << user.username << " (exit " << result << ")\n";
        }
    }

    std::cout << '\n';
    if (failures == 0) {
        std::cout << GREEN << "Done." << RESET << '\n';
    } else {
        std::cout << RED << "Done with failures: " << failures << RESET << '\n';
    }
    std::cout << "Press any key to exit.\n";
    read_key();
    return failures == 0 ? 0 : 1;
}

void move_selection(int& selected, int delta) {
    constexpr int maxField = 11;
    selected += delta;
    if (selected < 0) {
        selected = maxField;
    }
    if (selected > maxField) {
        selected = 0;
    }
}

void adjust_field(Options& options, int selected, int delta) {
    if (selected == 0) {
        options.many = !options.many;
        if (options.many) {
            if (options.count == 1) {
                options.count = 2;
            }
        }
    } else if (selected == 1 && options.many) {
        options.count = std::max(1, options.count + delta);
    } else if (selected == 5) {
        options.createHome = !options.createHome;
    } else if (selected == 9) {
        options.addSudoGroup = !options.addSudoGroup;
    }
}

bool edit_field(Options& options, int selected, int& exitCode) {
    options.message.clear();

    if (selected == 0 || selected == 5 || selected == 9) {
        adjust_field(options, selected, 1);
        return false;
    }
    if (selected == 1) {
        if (options.many) {
            options.count = std::max(1, options.count + 1);
        }
        return false;
    }
    if (selected == 2) options.usernameTemplate = edit_username_template(options.usernameTemplate);
    if (selected == 3) options.passwordTemplate = edit_value("Password template", options.passwordTemplate, true);
    if (selected == 4) options.fullNameTemplate = edit_value("Full name template", options.fullNameTemplate, false);
    if (selected == 6) options.shellTemplate = edit_value("Login shell", options.shellTemplate, false);
    if (selected == 7) options.homeTemplate = edit_value("Home path", options.homeTemplate, false);
    if (selected == 8) options.groupsTemplate = select_groups(options.groupsTemplate);

    if (selected == 10) {
        if (!validate_options(options)) {
            return false;
        }
        if (confirm_create(options)) {
            exitCode = run_creation(options);
            return true;
        }
    }

    if (selected == 11) {
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

    if (!command_exists("useradd") || !command_exists("chpasswd")) {
        std::cerr << RED << "ERROR:" << RESET << " missing useradd or chpasswd.\n";
        return 1;
    }

    Terminal terminal;
    Options options;
    int selected = 0;

    while (true) {
        draw(options, selected);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) {
            return 0;
        }
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
            if (edit_field(options, selected, exitCode)) {
                return exitCode;
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
