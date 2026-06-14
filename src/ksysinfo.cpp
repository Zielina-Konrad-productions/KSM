#include "main.h"

#include <cstdio>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

enum class Key { Enter, Escape, CtrlC, Character, Unknown };

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
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
    std::cout << "              ksysinfo\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "ksysinfo component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "ksysinfo [options]\n";
    std::cout << "Interactive system information dashboard.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Enter/r       Refresh\n";
    std::cout << "  q             Exit\n";
}

KeyPress read_key() {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) != 1) return {Key::Unknown, '\0'};
    if (c == 3) return {Key::CtrlC, '\0'};
    if (c == 27) return {Key::Escape, '\0'};
    if (c == '\n' || c == '\r') return {Key::Enter, '\0'};
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

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    pclose(pipe);
    return trim(output);
}

std::string file_value(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return trim(buffer.str());
}

std::string os_pretty_name() {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string value = line.substr(12);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return "unknown";
}

void row(const std::string& label, const std::string& value) {
    std::cout << "  " << CYAN << label << RESET;
    if (label.size() < 16) std::cout << std::string(16 - label.size(), ' ');
    std::cout << (value.empty() ? DIM + "unknown" + RESET : value) << '\n';
}

void draw() {
    clear_screen();
    banner();
    std::cout << CYAN << "Enter/r:" << RESET << " refresh  "
              << CYAN << "q:" << RESET << " exit\n\n";

    std::cout << BOLD << BLUE << "System" << RESET << '\n';
    row("OS", os_pretty_name());
    row("Kernel", command_output("uname -r"));
    row("Hostname", command_output("hostname"));
    row("Uptime", command_output("uptime -p 2>/dev/null"));

    std::cout << '\n' << BOLD << BLUE << "Resources" << RESET << '\n';
    row("CPU", command_output("awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//'"));
    row("Load", command_output("awk '{print $1\" \"$2\" \"$3}' /proc/loadavg"));
    row("Memory", command_output("free -h | awk '/Mem:/ {print $3\" used / \"$2\" total\"}'"));
    row("Root disk", command_output("df -h / | awk 'NR==2 {print $3\" used / \"$2\" total (\"$5\")\"}'"));

    std::cout << '\n' << BOLD << BLUE << "Network" << RESET << '\n';
    row("IPv4", command_output("hostname -I 2>/dev/null | awk '{print $1}'"));
    row("Default route", command_output("ip route 2>/dev/null | awk '/default/ {print $3\" via \"$5; exit}'"));

    std::cout << '\n' << BOLD << BLUE << "Health" << RESET << '\n';
    row("Failed units", command_output("systemctl --failed --no-legend 2>/dev/null | wc -l"));
    row("Logged users", command_output("who | wc -l"));
    std::cout << std::flush;
}

int run_tui() {
    Terminal terminal;
    while (true) {
        draw();
        const KeyPress key = read_key();
        if (key.key == Key::CtrlC || key.key == Key::Escape ||
            (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Enter || (key.key == Key::Character && key.value == 'r')) continue;
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
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'ksysinfo --help' to list options.\n";
        return 1;
    }
    return run_tui();
}
