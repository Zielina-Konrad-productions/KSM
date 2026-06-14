#include "main.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

enum class Key {
    Up,
    Down,
    Left,
    Right,
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

struct InterfaceEntry {
    std::string name;
    bool up = false;
};

struct Options {
    std::string interfaceName;
    bool enableInterface = true;
    bool dhcp = true;
    bool flushAddresses = true;
    std::string addressCidr;
    std::string gateway;
    std::string dnsServers;
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
        restore();
    }

    void restore() {
        if (restored_) return;
        std::cout << "\033[?25h\033[0m" << std::flush;
        if (rawEnabled_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
        }
        restored_ = true;
    }

private:
    termios oldTerm_ {};
    bool rawEnabled_ = false;
    bool restored_ = false;
};

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

void banner() {
    std::cout << BLUE;
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "              knetcfg\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "knetcfg component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "sudo knetcfg [options]\n";
    std::cout << "Interactive terminal GUI for live network interface configuration.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move between fields\n";
    std::cout << "  Left/Right    Toggle checkboxes\n";
    std::cout << "  Tab           Jump to Apply/Top\n";
    std::cout << "  Enter         Edit text, choose interface, toggle, or apply\n";
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

std::string dim_empty(const std::string& value) {
    return value.empty() ? DIM + "(empty)" + RESET : value;
}

std::string read_first_line(const fs::path& path) {
    std::ifstream file(path);
    std::string line;
    if (std::getline(file, line)) return trim(line);
    return "";
}

bool command_exists(const std::string& command);

std::vector<std::string> split_words(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;
    while (ss >> item) {
        result.push_back(item);
    }
    return result;
}

std::vector<InterfaceEntry> read_interfaces() {
    std::vector<InterfaceEntry> interfaces;
    const fs::path root("/sys/class/net");
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return interfaces;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        InterfaceEntry iface;
        iface.name = entry.path().filename().string();
        if (iface.name.empty() || iface.name == "lo") continue;
        iface.up = read_first_line(entry.path() / "operstate") == "up";
        interfaces.push_back(iface);
    }

    std::sort(interfaces.begin(), interfaces.end(), [](const InterfaceEntry& a, const InterfaceEntry& b) {
        return a.name < b.name;
    });
    return interfaces;
}

bool interface_exists(const std::string& name) {
    if (name.empty()) return false;
    return fs::exists(fs::path("/sys/class/net") / name);
}

ProcessResult run_process(const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid < 0) return {1};

    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
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

void draw_row(int row, int selected, const std::string& label, const std::string& value, bool disabled = false) {
    const bool active = row == selected;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (disabled) std::cout << DIM;
    std::cout << label;
    if (label.size() < 24) {
        std::cout << std::string(24 - label.size(), ' ');
    }
    std::cout << value << RESET << '\n';
}

void draw(const Options& options, int selected) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " apply/top  "
              << CYAN << "Enter:" << RESET << " edit/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    std::cout << BOLD << "Network interface" << RESET << '\n';
    draw_row(0, selected, "Interface", dim_empty(options.interfaceName));
    draw_row(1, selected, "Enable interface", yes_no(options.enableInterface));
    draw_row(2, selected, "Use DHCP", yes_no(options.dhcp));
    draw_row(3, selected, "Flush addresses", yes_no(options.flushAddresses));
    draw_row(4, selected, "IPv4/CIDR", dim_empty(options.addressCidr), options.dhcp);
    draw_row(5, selected, "Gateway", dim_empty(options.gateway), options.dhcp);
    draw_row(6, selected, "DNS servers", dim_empty(options.dnsServers), options.dhcp);
    draw_row(7, selected, "Apply config", "Enter");
    draw_row(8, selected, "Cancel", "Enter or q");

    std::cout << '\n' << DIM << "Static DNS uses resolvectl when available." << RESET << '\n';
    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
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

std::string select_interface(const std::string& current) {
    auto interfaces = read_interfaces();
    if (interfaces.empty()) return current;

    int cursor = 0;
    for (size_t i = 0; i < interfaces.size(); ++i) {
        if (interfaces[i].name == current) {
            cursor = static_cast<int>(i);
            break;
        }
    }

    while (true) {
        clear_screen();
        banner();
        std::cout << CYAN << "Select interface" << RESET << "\n\n";
        const int visible = 14;
        int start = std::max(0, cursor - visible / 2);
        if (start + visible > static_cast<int>(interfaces.size())) {
            start = std::max(0, static_cast<int>(interfaces.size()) - visible);
        }
        const int end = std::min<int>(static_cast<int>(interfaces.size()), start + visible);

        for (int i = start; i < end; ++i) {
            const bool active = i == cursor;
            std::cout << (active ? BLUE : "");
            std::cout << (active ? "> " : "  ");
            std::cout << interfaces[i].name << "  "
                      << (interfaces[i].up ? GREEN + std::string("up") : DIM + std::string("down"))
                      << RESET << '\n';
        }
        std::cout << "\nEnter selects, q cancels.\n" << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Up) {
            cursor = std::max(0, cursor - 1);
        } else if (key.key == Key::Down) {
            cursor = std::min<int>(static_cast<int>(interfaces.size()) - 1, cursor + 1);
        } else if (key.key == Key::Enter) {
            return interfaces[cursor].name;
        } else if (key.key == Key::Escape || key.key == Key::CtrlC ||
                   (key.key == Key::Character && key.value == 'q')) {
            return current;
        }
    }
}

void move_selection(int& selected, int delta) {
    constexpr int maxField = 8;
    selected += delta;
    if (selected < 0) selected = maxField;
    if (selected > maxField) selected = 0;
}

bool validate_options(Options& options) {
    if (!interface_exists(options.interfaceName)) {
        options.message = "Select a valid network interface.";
        return false;
    }
    if (!options.dhcp && trim(options.addressCidr).empty()) {
        options.message = "Static mode requires IPv4/CIDR, for example 192.168.1.20/24.";
        return false;
    }
    if (!command_exists("ip")) {
        options.message = "Missing required command: ip.";
        return false;
    }
    if (options.dhcp && !command_exists("dhclient")) {
        options.message = "DHCP mode requires dhclient. Use static config or install dhclient.";
        return false;
    }
    options.message.clear();
    return true;
}

bool confirm_apply(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to configure interface" << RESET << "\n\n";
    std::cout << "Interface       : " << options.interfaceName << '\n';
    std::cout << "Enable          : " << (options.enableInterface ? "yes" : "no") << '\n';
    std::cout << "Mode            : " << (options.dhcp ? "DHCP" : "static") << '\n';
    std::cout << "Flush addresses : " << (options.flushAddresses ? "yes" : "no") << '\n';
    if (!options.dhcp) {
        std::cout << "IPv4/CIDR       : " << options.addressCidr << '\n';
        std::cout << "Gateway         : " << (options.gateway.empty() ? "-" : options.gateway) << '\n';
        std::cout << "DNS servers     : " << (options.dnsServers.empty() ? "-" : options.dnsServers) << '\n';
    }
    std::cout << "\nPress " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to apply, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

bool apply_static_dns(const Options& options) {
    if (trim(options.dnsServers).empty()) return true;
    if (!command_exists("resolvectl")) {
        std::cout << YELLOW << "[!]" << RESET << " resolvectl not found, DNS not changed.\n";
        return true;
    }

    std::vector<std::string> args = {"resolvectl", "dns", options.interfaceName};
    const auto servers = split_words(options.dnsServers);
    args.insert(args.end(), servers.begin(), servers.end());
    return run_process(args).exitCode == 0;
}

int run_apply(const Options& options) {
    clear_screen();
    banner();

    int failures = 0;
    auto step = [&](const std::vector<std::string>& args, const std::string& label) {
        std::cout << CYAN << "[*]" << RESET << " " << label << "...\n";
        const int code = run_process(args).exitCode;
        if (code == 0) {
            std::cout << GREEN << "[+]" << RESET << " " << label << "\n";
        } else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " " << label << " failed (exit " << code << ")\n";
        }
    };

    step({"ip", "link", "set", "dev", options.interfaceName, options.enableInterface ? "up" : "down"},
         options.enableInterface ? "Interface enabled" : "Interface disabled");

    if (options.enableInterface) {
        if (options.flushAddresses) {
            step({"ip", "addr", "flush", "dev", options.interfaceName}, "Addresses flushed");
        }

        if (options.dhcp) {
            if (command_exists("dhclient")) {
                run_process({"dhclient", "-r", options.interfaceName});
                step({"dhclient", options.interfaceName}, "DHCP lease requested");
            }
        } else {
            step({"ip", "addr", "add", options.addressCidr, "dev", options.interfaceName}, "Static address added");
            if (!trim(options.gateway).empty()) {
                step({"ip", "route", "replace", "default", "via", options.gateway, "dev", options.interfaceName},
                     "Default route updated");
            }
            if (!apply_static_dns(options)) {
                ++failures;
                std::cout << RED << "[x]" << RESET << " DNS update failed\n";
            }
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

bool edit_field(Options& options, int selected, int& exitCode) {
    options.message.clear();

    if (selected == 0) options.interfaceName = select_interface(options.interfaceName);
    if (selected == 1) options.enableInterface = !options.enableInterface;
    if (selected == 2) options.dhcp = !options.dhcp;
    if (selected == 3) options.flushAddresses = !options.flushAddresses;
    if (selected == 4 && !options.dhcp) options.addressCidr = edit_value("IPv4/CIDR", options.addressCidr);
    if (selected == 5 && !options.dhcp) options.gateway = edit_value("Gateway", options.gateway);
    if (selected == 6 && !options.dhcp) options.dnsServers = edit_value("DNS servers", options.dnsServers);

    if (selected == 7) {
        if (!validate_options(options)) return false;
        if (confirm_apply(options)) {
            exitCode = run_apply(options);
            return true;
        }
    }

    if (selected == 8) {
        exitCode = 0;
        return true;
    }
    return false;
}

int run_tui(Options options) {
    if (geteuid() != 0) {
        banner();
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    const auto interfaces = read_interfaces();
    if (options.interfaceName.empty() && !interfaces.empty()) {
        options.interfaceName = interfaces.front().name;
    }

    Terminal terminal;
    int selected = 0;
    int exitCode = 0;

    while (true) {
        draw(options, selected);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_selection(selected, -1);
        } else if (key.key == Key::Down) {
            move_selection(selected, 1);
        } else if (key.key == Key::Tab) {
            selected = selected < 7 ? 7 : 0;
        } else if (key.key == Key::Left || key.key == Key::Right) {
            if (selected >= 1 && selected <= 3) {
                edit_field(options, selected, exitCode);
            }
        } else if (key.key == Key::Enter) {
            if (edit_field(options, selected, exitCode)) return exitCode;
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
        std::cerr << "run 'sudo knetcfg --help' to list options.\n";
        return 1;
    }

    return run_tui(options);
}
