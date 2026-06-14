#include "main.h"

#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr const char* kSshdConfig = "/etc/ssh/sshd_config";

enum class Key { Up, Down, Left, Right, Tab, Enter, Backspace, Escape, CtrlC, Character, Unknown };

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

struct Options {
    std::string port = "22";
    std::string permitRootLogin = "prohibit-password";
    std::string passwordAuthentication = "no";
    std::string pubkeyAuthentication = "yes";
    std::string x11Forwarding = "no";
    bool restartService = true;
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
    std::cout << "                kssh\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kssh component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kssh [options]\n";
    std::cout << "Interactive sshd_config helper.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Left/Right    Change choices\n";
    std::cout << "  Tab           Jump to Apply/Top\n";
    std::cout << "  Enter         Edit/toggle/action\n";
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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string yes_no(bool value) {
    return value ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
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

std::string value_for_line(const std::string& line, const std::string& key) {
    std::string clean = trim(line);
    if (clean.empty()) return "";
    if (clean[0] == '#') clean = trim(clean.substr(1));

    std::istringstream ss(clean);
    std::string currentKey;
    std::string value;
    ss >> currentKey >> value;
    if (lower(currentKey) == lower(key)) return value;
    return "";
}

void load_config(Options& options) {
    std::ifstream file(kSshdConfig);
    if (!file) {
        options.message = "Could not read sshd_config, using defaults.";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (const std::string value = value_for_line(line, "Port"); !value.empty()) options.port = value;
        if (const std::string value = value_for_line(line, "PermitRootLogin"); !value.empty()) options.permitRootLogin = value;
        if (const std::string value = value_for_line(line, "PasswordAuthentication"); !value.empty()) options.passwordAuthentication = value;
        if (const std::string value = value_for_line(line, "PubkeyAuthentication"); !value.empty()) options.pubkeyAuthentication = value;
        if (const std::string value = value_for_line(line, "X11Forwarding"); !value.empty()) options.x11Forwarding = value;
    }
    options.message = "Loaded sshd_config.";
}

void cycle(std::string& value, const std::vector<std::string>& values, int delta) {
    auto it = std::find(values.begin(), values.end(), value);
    int index = it == values.end() ? 0 : static_cast<int>(it - values.begin());
    index += delta;
    if (index < 0) index = static_cast<int>(values.size()) - 1;
    if (index >= static_cast<int>(values.size())) index = 0;
    value = values[index];
}

void draw_field(int row, int cursor, const std::string& label, const std::string& value) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    std::cout << label;
    if (label.size() < 24) std::cout << std::string(24 - label.size(), ' ');
    std::cout << value << RESET << '\n';
}

void draw(const Options& options, int cursor) {
    clear_screen();
    banner();
    std::cout << CYAN << "Config:" << RESET << " " << kSshdConfig << '\n';
    std::cout << CYAN << "Arrows:" << RESET << " move/change  "
              << CYAN << "Tab:" << RESET << " apply/top  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    draw_field(0, cursor, "Port", options.port);
    draw_field(1, cursor, "PermitRootLogin", options.permitRootLogin);
    draw_field(2, cursor, "PasswordAuthentication", options.passwordAuthentication);
    draw_field(3, cursor, "PubkeyAuthentication", options.pubkeyAuthentication);
    draw_field(4, cursor, "X11Forwarding", options.x11Forwarding);
    draw_field(5, cursor, "Restart ssh service", yes_no(options.restartService));
    draw_field(6, cursor, "Reload config", "Enter");
    draw_field(7, cursor, "Apply changes", GREEN + "Enter" + RESET);
    draw_field(8, cursor, "Cancel", "Enter or q");

    if (!options.message.empty()) std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    std::cout << std::flush;
}

std::string edit_port(std::string value) {
    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Editing:" << RESET << " Port\n";
        std::cout << "Enter saves, Esc cancels, Backspace deletes.\n\n";
        std::cout << BLUE << "Port" << RESET << ": " << value << std::flush;
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

bool validate(Options& options) {
    if (options.port.empty() || !std::all_of(options.port.begin(), options.port.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        options.message = "Port must be a number.";
        return false;
    }
    const int port = std::stoi(options.port);
    if (port < 1 || port > 65535) {
        options.message = "Port must be between 1 and 65535.";
        return false;
    }
    options.message.clear();
    return true;
}

void set_directive(std::vector<std::string>& lines, const std::string& key, const std::string& value) {
    bool found = false;
    for (auto& line : lines) {
        std::string clean = trim(line);
        if (clean.empty()) continue;
        if (clean[0] == '#') clean = trim(clean.substr(1));
        std::istringstream ss(clean);
        std::string currentKey;
        ss >> currentKey;
        if (lower(currentKey) == lower(key)) {
            if (!found) {
                line = key + " " + value;
                found = true;
            } else {
                line = "#" + line;
            }
        }
    }
    if (!found) lines.push_back(key + " " + value);
}

bool confirm_apply(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to update SSH config" << RESET << "\n\n";
    std::cout << "Port                   : " << options.port << '\n';
    std::cout << "PermitRootLogin        : " << options.permitRootLogin << '\n';
    std::cout << "PasswordAuthentication : " << options.passwordAuthentication << '\n';
    std::cout << "PubkeyAuthentication   : " << options.pubkeyAuthentication << '\n';
    std::cout << "X11Forwarding          : " << options.x11Forwarding << '\n';
    std::cout << "Restart ssh service    : " << (options.restartService ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to apply, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int apply_config(const Options& options) {
    clear_screen();
    banner();

    std::ifstream input(kSshdConfig);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);

    set_directive(lines, "Port", options.port);
    set_directive(lines, "PermitRootLogin", options.permitRootLogin);
    set_directive(lines, "PasswordAuthentication", options.passwordAuthentication);
    set_directive(lines, "PubkeyAuthentication", options.pubkeyAuthentication);
    set_directive(lines, "X11Forwarding", options.x11Forwarding);

    std::cout << CYAN << "[*]" << RESET << " Creating backup...\n";
    run_process({"cp", kSshdConfig, std::string(kSshdConfig) + ".ksm.bak"});

    std::ofstream output(kSshdConfig);
    if (!output) {
        std::cout << RED << "[x]" << RESET << " Could not write sshd_config.\n";
        std::cout << "Press any key to exit.\n";
        read_key();
        return 1;
    }
    for (const auto& next : lines) output << next << '\n';
    std::cout << GREEN << "[+]" << RESET << " sshd_config updated.\n";

    int failures = 0;
    if (options.restartService) {
        std::cout << CYAN << "[*]" << RESET << " Restarting SSH service...\n";
        int code = run_process({"systemctl", "restart", "ssh"}).exitCode;
        if (code != 0) code = run_process({"systemctl", "restart", "sshd"}).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " SSH service restarted.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " Service restart failed.\n";
        }
    }

    std::cout << '\n' << (failures == 0 ? GREEN + "Done." : RED + "Done with failures: " + std::to_string(failures))
              << RESET << '\n';
    std::cout << "Press any key to exit.\n";
    read_key();
    return failures == 0 ? 0 : 1;
}

void move_cursor(int& cursor, int delta) {
    constexpr int maxRow = 8;
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

    load_config(options);
    Terminal terminal;
    int cursor = 0;

    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();
        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1);
        else if (key.key == Key::Down) move_cursor(cursor, 1);
        else if (key.key == Key::Tab) cursor = cursor < 7 ? 7 : 0;
        else if (key.key == Key::Left || key.key == Key::Right) {
            const int delta = key.key == Key::Right ? 1 : -1;
            if (cursor == 1) cycle(options.permitRootLogin, {"no", "prohibit-password", "yes"}, delta);
            else if (cursor == 2) cycle(options.passwordAuthentication, {"no", "yes"}, delta);
            else if (cursor == 3) cycle(options.pubkeyAuthentication, {"yes", "no"}, delta);
            else if (cursor == 4) cycle(options.x11Forwarding, {"no", "yes"}, delta);
            else if (cursor == 5) options.restartService = !options.restartService;
        } else if (key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) options.port = edit_port(options.port);
            else if (cursor == 1) cycle(options.permitRootLogin, {"no", "prohibit-password", "yes"}, 1);
            else if (cursor == 2) cycle(options.passwordAuthentication, {"no", "yes"}, 1);
            else if (cursor == 3) cycle(options.pubkeyAuthentication, {"yes", "no"}, 1);
            else if (cursor == 4) cycle(options.x11Forwarding, {"no", "yes"}, 1);
            else if (cursor == 5) options.restartService = !options.restartService;
            else if (cursor == 6) load_config(options);
            else if (cursor == 7) {
                if (validate(options) && confirm_apply(options)) return apply_config(options);
            } else if (cursor == 8) return 0;
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
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kssh --help' to list options.\n";
        return 1;
    }
    return run_tui({});
}
