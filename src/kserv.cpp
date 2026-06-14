#include "main.h"

#include <cstdio>
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

struct Service {
    std::string name;
    std::string load;
    std::string active;
    std::string sub;
    std::string description;
};

struct Options {
    std::string selectedService;
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
    std::cout << "               kserv\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kserv component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kserv [options]\n";
    std::cout << "Interactive systemd service manager.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Tab           Jump to actions/Top\n";
    std::cout << "  Enter         Select service or run action\n";
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

std::vector<Service> read_services() {
    std::vector<Service> services;
    FILE* pipe = popen("systemctl list-units --type=service --all --no-legend --no-pager 2>/dev/null", "r");
    if (!pipe) return services;

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        std::istringstream ss(line);
        Service service;
        ss >> service.name >> service.load >> service.active >> service.sub;
        std::getline(ss, service.description);
        const auto begin = service.description.find_first_not_of(" \t\r\n");
        if (begin != std::string::npos) service.description = service.description.substr(begin);
        if (!service.name.empty()) services.push_back(service);
    }
    pclose(pipe);
    return services;
}

std::string service_color(const Service& service) {
    if (service.active == "active") return GREEN;
    if (service.active == "failed") return RED;
    return DIM;
}

void draw_row(int row, int cursor, const std::string& text, bool action = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (action) std::cout << GREEN;
    std::cout << text << RESET << '\n';
}

void draw(const std::vector<Service>& services, const Options& options, int cursor, int offset) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " actions/top  "
              << CYAN << "Enter:" << RESET << " select/run  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    constexpr int pageSize = 11;
    const int end = std::min<int>(static_cast<int>(services.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        const auto& service = services[i];
        std::ostringstream row;
        row << (options.selectedService == service.name ? GREEN + "[x] " + RESET : DIM + "[ ] " + RESET);
        row << service.name << "  " << service_color(service) << service.active << RESET << "/" << service.sub;
        if (!service.description.empty()) row << "  " << service.description;
        draw_row(i, cursor, row.str());
    }
    if (services.empty()) std::cout << "  No systemd services found.\n";

    const int base = static_cast<int>(services.size());
    std::cout << '\n' << BOLD << "Service: " << RESET
              << (options.selectedService.empty() ? DIM + "(none)" + RESET : CYAN + options.selectedService + RESET) << '\n';
    draw_row(base, cursor, "Start                 Enter", true);
    draw_row(base + 1, cursor, "Stop                  Enter", true);
    draw_row(base + 2, cursor, "Restart               Enter", true);
    draw_row(base + 3, cursor, "Enable                Enter", true);
    draw_row(base + 4, cursor, "Disable               Enter", true);
    draw_row(base + 5, cursor, "Reload list           Enter");
    draw_row(base + 6, cursor, "Cancel                Enter or q");

    if (!options.message.empty()) std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    std::cout << std::flush;
}

bool confirm_action(const std::string& action, const std::string& service) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to run systemctl " << action << RESET << "\n\n";
    std::cout << "Service: " << CYAN << service << RESET << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to continue, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

void move_cursor(int& cursor, int delta, int maxRow) {
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

void run_action(Options& options, const std::string& action) {
    if (options.selectedService.empty()) {
        options.message = "Select service first.";
        return;
    }
    if (geteuid() != 0) {
        options.message = "Run with sudo for service actions.";
        return;
    }
    if (!confirm_action(action, options.selectedService)) return;
    const int code = run_process({"systemctl", action, options.selectedService}).exitCode;
    options.message = code == 0 ? "Action completed." : "systemctl failed (exit " + std::to_string(code) + ").";
}

int run_tui(Options options) {
    auto services = read_services();
    Terminal terminal;
    int cursor = 0;
    int offset = 0;

    while (true) {
        const int maxRow = static_cast<int>(services.size()) + 6;
        draw(services, options, cursor, offset);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1, maxRow);
        else if (key.key == Key::Down) move_cursor(cursor, 1, maxRow);
        else if (key.key == Key::Tab) cursor = cursor < static_cast<int>(services.size()) ? static_cast<int>(services.size()) : 0;
        else if (key.key == Key::Enter) {
            const int base = static_cast<int>(services.size());
            options.message.clear();
            if (cursor >= 0 && cursor < static_cast<int>(services.size())) {
                options.selectedService = services[cursor].name;
            } else if (cursor == base) run_action(options, "start");
            else if (cursor == base + 1) run_action(options, "stop");
            else if (cursor == base + 2) run_action(options, "restart");
            else if (cursor == base + 3) run_action(options, "enable");
            else if (cursor == base + 4) run_action(options, "disable");
            else if (cursor == base + 5) {
                services = read_services();
                cursor = 0;
                offset = 0;
                options.message = "Service list reloaded.";
            } else if (cursor == base + 6) return 0;
        }

        constexpr int pageSize = 11;
        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize && cursor < static_cast<int>(services.size())) offset = cursor - pageSize + 1;
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
        std::cerr << "run 'kserv --help' to list options.\n";
        return 1;
    }
    return run_tui(options);
}
