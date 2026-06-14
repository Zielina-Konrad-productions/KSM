#include "main.h"

#include <cstdio>
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

struct Options {
    std::string backend = "auto";
    std::string port;
    std::string protocol = "tcp";
    std::string action = "allow";
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
    std::cout << "             kfirewall\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kfirewall component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kfirewall [options]\n";
    std::cout << "Interactive ufw/firewalld helper.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Left/Right    Change choices\n";
    std::cout << "  Tab           Jump to actions/Top\n";
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

std::string command_output(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    pclose(pipe);
    return trim(output);
}

bool command_exists(const std::string& command) {
    return !command_output("command -v " + command + " 2>/dev/null").empty();
}

std::string detect_backend() {
    if (command_exists("ufw")) return "ufw";
    if (command_exists("firewall-cmd")) return "firewalld";
    return "none";
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
    if (label.size() < 18) std::cout << std::string(18 - label.size(), ' ');
    std::cout << value << RESET << '\n';
}

void draw(const Options& options, int cursor) {
    clear_screen();
    banner();
    const std::string backend = options.backend == "auto" ? detect_backend() : options.backend;
    std::cout << CYAN << "Backend:" << RESET << " " << backend << "  "
              << CYAN << "Arrows:" << RESET << " move/change  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    draw_field(0, cursor, "Backend", options.backend);
    draw_field(1, cursor, "Port", options.port.empty() ? DIM + "(empty)" + RESET : options.port);
    draw_field(2, cursor, "Protocol", options.protocol);
    draw_field(3, cursor, "Action", options.action);
    draw_field(4, cursor, "Apply rule", GREEN + "Enter" + RESET);
    draw_field(5, cursor, "Enable firewall", "Enter");
    draw_field(6, cursor, "Reload/status", "Enter");
    draw_field(7, cursor, "Cancel", "Enter or q");

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
        } else if (key.key == Key::Character && std::isdigit(static_cast<unsigned char>(key.value))) {
            value.push_back(key.value);
        }
    }
}

bool confirm(const std::string& title, const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << title << RESET << "\n\n";
    std::cout << "Backend : " << (options.backend == "auto" ? detect_backend() : options.backend) << '\n';
    std::cout << "Rule    : " << options.action << " " << options.port << "/" << options.protocol << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to continue, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

std::string apply_rule(const Options& options) {
    if (geteuid() != 0) return "Run with sudo.";
    const std::string backend = options.backend == "auto" ? detect_backend() : options.backend;
    if (backend == "none") return "No ufw or firewalld found.";
    if (options.port.empty()) return "Port is required.";
    if (!confirm("Ready to apply firewall rule", options)) return "";

    int code = 1;
    if (backend == "ufw") {
        if (options.action == "delete") code = run_process({"ufw", "delete", "allow", options.port + "/" + options.protocol}).exitCode;
        else code = run_process({"ufw", options.action, options.port + "/" + options.protocol}).exitCode;
    } else {
        const std::string flag = options.action == "delete" ? "--remove-port=" : "--add-port=";
        code = run_process({"firewall-cmd", "--permanent", flag + options.port + "/" + options.protocol}).exitCode;
        if (code == 0) run_process({"firewall-cmd", "--reload"});
    }
    return code == 0 ? "Rule applied." : "Firewall command failed (exit " + std::to_string(code) + ").";
}

std::string enable_firewall(const Options& options) {
    if (geteuid() != 0) return "Run with sudo.";
    const std::string backend = options.backend == "auto" ? detect_backend() : options.backend;
    if (backend == "ufw") {
        const int code = run_process({"ufw", "--force", "enable"}).exitCode;
        return code == 0 ? "ufw enabled." : "ufw enable failed.";
    }
    if (backend == "firewalld") {
        int code = run_process({"systemctl", "enable", "--now", "firewalld"}).exitCode;
        return code == 0 ? "firewalld enabled." : "firewalld enable failed.";
    }
    return "No ufw or firewalld found.";
}

std::string reload_status(const Options& options) {
    const std::string backend = options.backend == "auto" ? detect_backend() : options.backend;
    if (backend == "ufw") return command_output("ufw status 2>/dev/null | head -n 12");
    if (backend == "firewalld") return command_output("firewall-cmd --list-all 2>/dev/null | head -n 12");
    return "No ufw or firewalld found.";
}

void move_cursor(int& cursor, int delta) {
    constexpr int maxRow = 7;
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

int run_tui(Options options) {
    Terminal terminal;
    int cursor = 0;
    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();
        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1);
        else if (key.key == Key::Down) move_cursor(cursor, 1);
        else if (key.key == Key::Tab) cursor = cursor < 4 ? 4 : 0;
        else if (key.key == Key::Left || key.key == Key::Right) {
            const int delta = key.key == Key::Right ? 1 : -1;
            if (cursor == 0) cycle(options.backend, {"auto", "ufw", "firewalld"}, delta);
            else if (cursor == 2) cycle(options.protocol, {"tcp", "udp"}, delta);
            else if (cursor == 3) cycle(options.action, {"allow", "deny", "delete"}, delta);
        } else if (key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) cycle(options.backend, {"auto", "ufw", "firewalld"}, 1);
            else if (cursor == 1) options.port = edit_value("Port", options.port);
            else if (cursor == 2) cycle(options.protocol, {"tcp", "udp"}, 1);
            else if (cursor == 3) cycle(options.action, {"allow", "deny", "delete"}, 1);
            else if (cursor == 4) options.message = apply_rule(options);
            else if (cursor == 5) options.message = enable_firewall(options);
            else if (cursor == 6) options.message = reload_status(options);
            else if (cursor == 7) return 0;
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
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kfirewall --help' to list options.\n";
        return 1;
    }
    return run_tui(options);
}
