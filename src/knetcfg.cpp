#include "main.h"

#include <cerrno>
#include <cstdio>
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
    bool persistent = true;
    bool dhcp = true;
    bool flushAddresses = true;
    std::string addressCidr;
    std::string gateway;
    std::string dnsServers;
    std::string message;
};

struct ProcessResult {
    int exitCode = 1;
    std::string output;
};

struct ProcessConfig {
    bool captureStdout = false;
    bool quietStderr = false;
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
    std::cout << "Interactive terminal GUI for network interface configuration.\n";
    std::cout << "Loads current IP/gateway/DNS and can save persistent config.\n\n";
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

void redirect_to_dev_null(int fd) {
    FILE* file = fopen("/dev/null", fd == STDIN_FILENO ? "r" : "w");
    if (!file) return;
    dup2(fileno(file), fd);
    fclose(file);
}

ProcessResult run_process(const std::vector<std::string>& args, const ProcessConfig& config = {}) {
    if (args.empty()) return {};

    int pipeFd[2] = {-1, -1};
    if (config.captureStdout && pipe(pipeFd) != 0) {
        return {};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        if (pipeFd[0] >= 0) close(pipeFd[0]);
        if (pipeFd[1] >= 0) close(pipeFd[1]);
        return {};
    }

    if (pid == 0) {
        if (config.captureStdout) {
            close(pipeFd[0]);
            dup2(pipeFd[1], STDOUT_FILENO);
            close(pipeFd[1]);
        }
        if (config.quietStderr) {
            redirect_to_dev_null(STDERR_FILENO);
        }

        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    ProcessResult result;
    if (config.captureStdout) {
        close(pipeFd[1]);
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(pipeFd[0], buffer, sizeof(buffer));
            if (count > 0) {
                result.output.append(buffer, static_cast<size_t>(count));
                continue;
            }
            if (count == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        close(pipeFd[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return result;
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    result.output = trim(result.output);
    return result;
}

bool command_exists(const std::string& command) {
    ProcessConfig config;
    config.captureStdout = true;
    config.quietStderr = true;
    const ProcessResult result = run_process({"sh", "-c", "command -v " + command}, config);
    return result.exitCode == 0 && !result.output.empty();
}

std::string capture_output(const std::vector<std::string>& args) {
    ProcessConfig config;
    config.captureStdout = true;
    config.quietStderr = true;
    const ProcessResult result = run_process(args, config);
    return result.exitCode == 0 ? result.output : "";
}

std::string first_ipv4_cidr(const std::string& interfaceName) {
    const std::string output = capture_output({"ip", "-o", "-4", "addr", "show", "dev", interfaceName});
    std::stringstream ss(output);
    std::string token;
    while (ss >> token) {
        if (token == "inet") {
            std::string address;
            ss >> address;
            return trim(address);
        }
    }
    return "";
}

std::string default_gateway(const std::string& interfaceName) {
    const std::string output = capture_output({"ip", "route", "show", "default", "dev", interfaceName});
    std::stringstream ss(output);
    std::string token;
    while (ss >> token) {
        if (token == "via") {
            std::string gateway;
            ss >> gateway;
            return trim(gateway);
        }
    }
    return "";
}

std::string active_nm_connection(const std::string& interfaceName);

std::string current_dns_servers(const std::string& interfaceName) {
    if (command_exists("resolvectl")) {
        const std::string output = capture_output({"resolvectl", "dns", interfaceName});
        const auto pos = output.find(':');
        if (pos != std::string::npos) return trim(output.substr(pos + 1));
    }

    if (command_exists("nmcli")) {
        const std::string connection = active_nm_connection(interfaceName);
        if (!connection.empty()) {
            const std::string dns = capture_output({"nmcli", "-g", "ipv4.dns", "connection", "show", connection});
            if (!trim(dns).empty()) return trim(dns);
        }
    }

    std::ifstream file("/etc/resolv.conf");
    std::string line;
    std::string servers;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.rfind("nameserver ", 0) != 0) continue;
        const std::string server = trim(line.substr(11));
        if (server.empty()) continue;
        if (!servers.empty()) servers += " ";
        servers += server;
    }
    return servers;
}

std::string active_nm_connection(const std::string& interfaceName) {
    if (!command_exists("nmcli")) return "";
    const std::string output = capture_output({"nmcli", "-t", "-f", "NAME,DEVICE", "connection", "show", "--active"});
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto pos = line.rfind(':');
        if (pos == std::string::npos) continue;
        const std::string name = line.substr(0, pos);
        const std::string device = line.substr(pos + 1);
        if (device == interfaceName) return name;
    }
    return "";
}

std::string nm_ipv4_method(const std::string& connectionName) {
    if (connectionName.empty()) return "";
    return capture_output({"nmcli", "-g", "ipv4.method", "connection", "show", connectionName});
}

void load_current_settings(Options& options) {
    if (!interface_exists(options.interfaceName)) return;

    options.enableInterface = read_first_line(fs::path("/sys/class/net") / options.interfaceName / "operstate") == "up";
    options.addressCidr = first_ipv4_cidr(options.interfaceName);
    options.gateway = default_gateway(options.interfaceName);
    options.dnsServers = current_dns_servers(options.interfaceName);
    const std::string method = nm_ipv4_method(active_nm_connection(options.interfaceName));
    if (method == "auto") {
        options.dhcp = true;
    } else if (method == "manual") {
        options.dhcp = false;
    } else {
        options.dhcp = options.addressCidr.empty();
    }
    options.message = "Loaded current settings for " + options.interfaceName + ".";
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
    draw_row(2, selected, "Save persistently", yes_no(options.persistent));
    draw_row(3, selected, "Use DHCP", yes_no(options.dhcp));
    draw_row(4, selected, "Flush addresses", yes_no(options.flushAddresses));
    draw_row(5, selected, "IPv4/CIDR", dim_empty(options.addressCidr), options.dhcp);
    draw_row(6, selected, "Gateway", dim_empty(options.gateway), options.dhcp);
    draw_row(7, selected, "DNS servers", dim_empty(options.dnsServers), options.dhcp);
    draw_row(8, selected, "Apply config", "Enter");
    draw_row(9, selected, "Cancel", "Enter or q");

    std::cout << '\n' << DIM << "Persistent save uses NetworkManager, netplan, or systemd-networkd." << RESET << '\n';
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
    constexpr int maxField = 9;
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
    if (options.dhcp && !command_exists("dhclient") && active_nm_connection(options.interfaceName).empty()) {
        options.message = "DHCP mode requires dhclient or an active NetworkManager connection.";
        return false;
    }
    if (options.persistent &&
        !command_exists("nmcli") &&
        !command_exists("netplan") &&
        !fs::is_directory("/etc/systemd")) {
        options.message = "Persistent save needs nmcli, netplan, or systemd-networkd.";
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
    std::cout << "Save persistent : " << (options.persistent ? "yes" : "no") << '\n';
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

bool write_resolv_conf(const std::string& dnsServers) {
    const auto servers = split_words(dnsServers);
    if (servers.empty()) return true;

    std::ofstream file("/etc/resolv.conf", std::ios::trunc);
    if (!file.is_open()) return false;
    file << "# Generated by KSM knetcfg\n";
    for (const auto& server : servers) {
        file << "nameserver " << server << '\n';
    }
    return file.good();
}

bool apply_static_dns(const Options& options) {
    if (trim(options.dnsServers).empty()) return true;
    const auto servers = split_words(options.dnsServers);

    if (command_exists("resolvectl")) {
        std::vector<std::string> args = {"resolvectl", "dns", options.interfaceName};
        args.insert(args.end(), servers.begin(), servers.end());
        if (run_process(args).exitCode == 0) return true;
    }

    if (command_exists("nmcli")) {
        const std::string connection = active_nm_connection(options.interfaceName);
        if (!connection.empty() &&
            run_process({"nmcli", "connection", "modify", connection, "ipv4.dns", options.dnsServers}).exitCode == 0 &&
            run_process({"nmcli", "connection", "up", connection}).exitCode == 0) {
            return true;
        }
    }

    return write_resolv_conf(options.dnsServers);
}

std::string safe_filename(std::string value) {
    for (char& ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
        if (!ok) ch = '_';
    }
    return value;
}

std::string yaml_dns_list(const std::string& dnsServers) {
    const auto servers = split_words(dnsServers);
    std::string result = "[";
    for (size_t i = 0; i < servers.size(); ++i) {
        if (i > 0) result += ", ";
        result += servers[i];
    }
    result += "]";
    return result;
}

bool save_with_nmcli(const Options& options) {
    const std::string connection = active_nm_connection(options.interfaceName);
    if (connection.empty()) return false;

    if (!options.enableInterface) {
        return run_process({"nmcli", "connection", "modify", connection, "connection.autoconnect", "no"}).exitCode == 0;
    }

    std::vector<std::string> args = {
        "nmcli", "connection", "modify", connection,
        "connection.autoconnect", "yes",
        "ipv4.method", options.dhcp ? "auto" : "manual"
    };

    if (options.dhcp) {
        args.insert(args.end(), {"ipv4.addresses", "", "ipv4.gateway", "", "ipv4.dns", ""});
    } else {
        args.insert(args.end(), {"ipv4.addresses", options.addressCidr});
        if (!trim(options.gateway).empty()) {
            args.insert(args.end(), {"ipv4.gateway", options.gateway});
        }
        if (!trim(options.dnsServers).empty()) {
            args.insert(args.end(), {"ipv4.dns", options.dnsServers});
        }
    }
    return run_process(args).exitCode == 0;
}

bool save_with_netplan(const Options& options) {
    if (!command_exists("netplan")) return false;

    std::error_code ec;
    fs::create_directories("/etc/netplan", ec);
    if (ec) return false;

    const fs::path path = fs::path("/etc/netplan") / ("99-ksm-" + safe_filename(options.interfaceName) + ".yaml");
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;

    file << "network:\n";
    file << "  version: 2\n";
    file << "  ethernets:\n";
    file << "    " << options.interfaceName << ":\n";
    file << "      optional: true\n";
    if (!options.enableInterface) {
        file << "      dhcp4: false\n";
    } else if (options.dhcp) {
        file << "      dhcp4: true\n";
    } else {
        file << "      dhcp4: false\n";
        file << "      addresses:\n";
        file << "        - " << options.addressCidr << "\n";
        if (!trim(options.gateway).empty()) {
            file << "      routes:\n";
            file << "        - to: default\n";
            file << "          via: " << options.gateway << "\n";
        }
        if (!trim(options.dnsServers).empty()) {
            file << "      nameservers:\n";
            file << "        addresses: " << yaml_dns_list(options.dnsServers) << "\n";
        }
    }
    return file.good();
}

bool save_with_systemd_networkd(const Options& options) {
    std::error_code ec;
    fs::create_directories("/etc/systemd/network", ec);
    if (ec) return false;

    const fs::path path = fs::path("/etc/systemd/network") / ("10-ksm-" + safe_filename(options.interfaceName) + ".network");
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;

    file << "[Match]\n";
    file << "Name=" << options.interfaceName << "\n\n";
    file << "[Network]\n";
    if (!options.enableInterface) {
        file << "ConfigureWithoutCarrier=no\n";
    } else if (options.dhcp) {
        file << "DHCP=ipv4\n";
    } else {
        file << "Address=" << options.addressCidr << "\n";
        if (!trim(options.gateway).empty()) {
            file << "Gateway=" << options.gateway << "\n";
        }
        for (const auto& dns : split_words(options.dnsServers)) {
            file << "DNS=" << dns << "\n";
        }
    }
    return file.good();
}

bool save_persistent_config(const Options& options) {
    if (!options.persistent) return true;
    if (command_exists("nmcli") && save_with_nmcli(options)) return true;
    if (save_with_netplan(options)) return true;
    return save_with_systemd_networkd(options);
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
            } else {
                const std::string connection = active_nm_connection(options.interfaceName);
                if (!connection.empty()) {
                    if (options.persistent) save_with_nmcli(options);
                    step({"nmcli", "connection", "up", connection}, "DHCP connection reloaded");
                } else {
                    ++failures;
                    std::cout << RED << "[x]" << RESET << " No DHCP backend available\n";
                }
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

    if (options.persistent) {
        std::cout << CYAN << "[*]" << RESET << " Saving persistent config...\n";
        if (save_persistent_config(options)) {
            std::cout << GREEN << "[+]" << RESET << " Persistent config saved.\n";
        } else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " Persistent config failed.\n";
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

    if (selected == 0) {
        const std::string previous = options.interfaceName;
        options.interfaceName = select_interface(options.interfaceName);
        if (options.interfaceName != previous) {
            load_current_settings(options);
        }
    }
    if (selected == 1) options.enableInterface = !options.enableInterface;
    if (selected == 2) options.persistent = !options.persistent;
    if (selected == 3) options.dhcp = !options.dhcp;
    if (selected == 4) options.flushAddresses = !options.flushAddresses;
    if (selected == 5 && !options.dhcp) options.addressCidr = edit_value("IPv4/CIDR", options.addressCidr);
    if (selected == 6 && !options.dhcp) options.gateway = edit_value("Gateway", options.gateway);
    if (selected == 7 && !options.dhcp) options.dnsServers = edit_value("DNS servers", options.dnsServers);

    if (selected == 8) {
        if (!validate_options(options)) return false;
        if (confirm_apply(options)) {
            exitCode = run_apply(options);
            return true;
        }
    }

    if (selected == 9) {
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
    load_current_settings(options);

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
            selected = selected < 8 ? 8 : 0;
        } else if (key.key == Key::Left || key.key == Key::Right) {
            if (selected >= 1 && selected <= 4) {
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
