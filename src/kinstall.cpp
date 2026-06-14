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
    Enter,
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
    std::string sourcePath;
    std::string targetPath = "/opt/KSM";
    std::string binPath = "/usr/bin";
    bool installDependencies = true;
    bool replaceExisting = true;
    bool buildProject = true;
    bool linkCommands = true;
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
        if (rawEnabled_) tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm_);
        restored_ = true;
    }

private:
    termios oldTerm_ {};
    bool rawEnabled_ = false;
    bool restored_ = false;
};

void clear_screen() {
    std::cout << "\033[H\033[J";
}

void banner() {
    std::cout << BLUE;
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "              Installer\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "KSM installer version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "sudo ./INSTALL.sh\n";
    std::cout << "Interactive terminal GUI for installing KSM.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Enter         Toggle option or run action\n";
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
    if (c == '\n' || c == '\r') return {Key::Enter, '\0'};
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

void draw_row(int row, int cursor, const std::string& text, bool action = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (action) std::cout << GREEN;
    std::cout << text << RESET << '\n';
}

void draw(const Options& options, int cursor) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Enter:" << RESET << " toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    std::cout << BOLD << "Install settings" << RESET << "\n";
    std::cout << "  Source: " << options.sourcePath << '\n';
    std::cout << "  Target: " << options.targetPath << '\n';
    std::cout << "  Links : " << options.binPath << "\n\n";

    draw_row(0, cursor, "Install dependencies  " + yes_no(options.installDependencies));
    draw_row(1, cursor, "Replace existing      " + yes_no(options.replaceExisting));
    draw_row(2, cursor, "Build C++ programs    " + yes_no(options.buildProject));
    draw_row(3, cursor, "Link commands         " + yes_no(options.linkCommands));
    draw_row(4, cursor, "Start installation    Enter", true);
    draw_row(5, cursor, "Cancel                Enter or q");

    std::cout << "\nCommands after install: ksm, khome, kupgr, kuninstall, kgroupadd, kgroupdel, kuseradd, kuserdel\n";
    if (!options.message.empty()) std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    std::cout << std::flush;
}

ProcessResult run_process(const std::vector<std::string>& args, const std::string& cwd = "") {
    const pid_t pid = fork();
    if (pid < 0) return {1};

    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
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

std::string detect_package_manager() {
    if (command_exists("apt-get")) return "apt";
    if (command_exists("zypper")) return "zypper";
    if (command_exists("dnf")) return "dnf";
    return "unknown";
}

std::vector<std::string> dependencies_for(const std::string& pm) {
    if (pm == "apt") return {"g++", "sudo", "coreutils", "nano", "passwd"};
    if (pm == "zypper") return {"gcc-c++", "sudo", "coreutils", "nano", "shadow"};
    if (pm == "dnf") return {"gcc-c++", "sudo", "coreutils", "nano", "shadow-utils"};
    return {};
}

bool required_files_exist(const Options& options) {
    return fs::is_directory(fs::path(options.sourcePath) / "src") &&
           fs::is_regular_file(fs::path(options.sourcePath) / "src" / "build.sh") &&
           fs::is_regular_file(fs::path(options.sourcePath) / "VERSION.txt") &&
           fs::is_regular_file(fs::path(options.sourcePath) / "kastiusz.conf");
}

bool normalize_script_line_endings(const fs::path& script) {
    std::ifstream input(script, std::ios::binary);
    if (!input.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto oldSize = content.size();
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
    if (content.size() == oldSize) return true;

    std::ofstream output(script, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;
    output << content;
    return output.good();
}

bool confirm_install(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to install KSM" << RESET << "\n\n";
    std::cout << "Source              : " << options.sourcePath << '\n';
    std::cout << "Target              : " << options.targetPath << '\n';
    std::cout << "Install dependencies: " << (options.installDependencies ? "yes" : "no") << '\n';
    std::cout << "Replace existing    : " << (options.replaceExisting ? "yes" : "no") << '\n';
    std::cout << "Build project       : " << (options.buildProject ? "yes" : "no") << '\n';
    std::cout << "Link commands       : " << (options.linkCommands ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to install, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

bool install_dependencies(const Options& options) {
    if (!options.installDependencies) {
        std::cout << YELLOW << "[!]" << RESET << " Dependency installation skipped.\n";
        return true;
    }

    const std::string pm = detect_package_manager();
    const auto deps = dependencies_for(pm);
    if (deps.empty()) {
        std::cout << YELLOW << "[!]" << RESET << " Unsupported package manager. Skipping dependencies.\n";
        return true;
    }

    std::cout << CYAN << "[*]" << RESET << " Installing dependencies with " << pm << "...\n";
    if (pm == "apt") {
        if (run_process({"apt-get", "update", "-y"}).exitCode != 0) return false;
        std::vector<std::string> args = {"apt-get", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        const bool ok = run_process(args).exitCode == 0;
        if (ok) std::cout << GREEN << "[+]" << RESET << " Dependencies installed.\n";
        return ok;
    }
    if (pm == "zypper") {
        if (run_process({"zypper", "--non-interactive", "refresh"}).exitCode != 0) return false;
        std::vector<std::string> args = {"zypper", "--non-interactive", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        const bool ok = run_process(args).exitCode == 0;
        if (ok) std::cout << GREEN << "[+]" << RESET << " Dependencies installed.\n";
        return ok;
    }
    if (pm == "dnf") {
        std::vector<std::string> args = {"dnf", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        const bool ok = run_process(args).exitCode == 0;
        if (ok) std::cout << GREEN << "[+]" << RESET << " Dependencies installed.\n";
        return ok;
    }
    return true;
}

bool copy_project(const Options& options) {
    const fs::path target(options.targetPath);
    if (fs::exists(target)) {
        if (!options.replaceExisting) {
            std::cout << YELLOW << "[!]" << RESET << " Target exists and replace is disabled.\n";
            return false;
        }
        std::cout << CYAN << "[*]" << RESET << " Removing old installation...\n";
        if (run_process({"rm", "-rf", options.targetPath}).exitCode != 0) return false;
        std::cout << GREEN << "[+]" << RESET << " Old installation removed.\n";
    }

    std::cout << CYAN << "[*]" << RESET << " Copying project to " << options.targetPath << "...\n";
    if (run_process({"mkdir", "-p", options.targetPath}).exitCode != 0) return false;
    const bool ok = run_process({"cp", "-R", options.sourcePath + "/.", options.targetPath + "/"}).exitCode == 0;
    if (ok) std::cout << GREEN << "[+]" << RESET << " Project copied.\n";
    return ok;
}

bool build_project(const Options& options) {
    if (!options.buildProject) {
        std::cout << YELLOW << "[!]" << RESET << " Build skipped.\n";
        return true;
    }

    const std::string script = options.targetPath + "/src/build.sh";
    std::cout << CYAN << "[*]" << RESET << " Building C++ programs...\n";
    if (!normalize_script_line_endings(script)) return false;
    if (run_process({"chmod", "+x", script}).exitCode != 0) return false;
    const bool ok = run_process({"bash", script}, options.targetPath + "/src").exitCode == 0;
    if (ok) std::cout << GREEN << "[+]" << RESET << " C++ programs built.\n";
    return ok;
}

bool link_commands(const Options& options) {
    if (!options.linkCommands) {
        std::cout << YELLOW << "[!]" << RESET << " Command linking skipped.\n";
        return true;
    }

    const fs::path binDir = fs::path(options.targetPath) / "bin";
    if (!fs::is_directory(binDir)) {
        std::cout << YELLOW << "[!]" << RESET << " Missing bin directory. Build first.\n";
        return false;
    }

    std::cout << CYAN << "[*]" << RESET << " Linking commands in " << options.binPath << "...\n";
    for (const auto& entry : fs::directory_iterator(binDir)) {
        if (!entry.is_regular_file()) continue;
        const std::string source = entry.path().string();
        const std::string link = (fs::path(options.binPath) / entry.path().filename()).string();
        if (run_process({"ln", "-sf", source, link}).exitCode != 0) return false;
    }
    std::cout << GREEN << "[+]" << RESET << " Commands linked.\n";
    return true;
}

int run_installation(Terminal& terminal, const Options& options) {
    clear_screen();
    banner();
    terminal.restore();

    if (!required_files_exist(options)) {
        std::cout << RED << "ERROR:" << RESET << " source path is not a KSM project.\n";
        return 1;
    }

    bool ok = true;
    ok = ok && install_dependencies(options);
    ok = ok && copy_project(options);
    ok = ok && build_project(options);
    ok = ok && link_commands(options);

    std::cout << '\n';
    if (ok) {
        std::cout << GREEN << "[+]" << RESET << " Installation complete.\n";
        std::cout << "Run: " << CYAN << "ksm" << RESET << ", " << CYAN << "khome" << RESET
                  << ", " << CYAN << "ksm upgrade" << RESET << ", "
                  << CYAN << "ksm groupadd" << RESET << ", "
                  << CYAN << "ksm useradd" << RESET << '\n';
        return 0;
    }

    std::cout << RED << "[x]" << RESET << " Installation failed.\n";
    return 1;
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

    if (!isatty(STDIN_FILENO)) {
        banner();
        std::cerr << RED << "ERROR:" << RESET << " interactive installer needs a real terminal.\n";
        return 1;
    }

    Terminal terminal;
    int cursor = 4;

    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, 5);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, 5);
        } else if (key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) {
                options.installDependencies = !options.installDependencies;
            } else if (cursor == 1) {
                options.replaceExisting = !options.replaceExisting;
            } else if (cursor == 2) {
                options.buildProject = !options.buildProject;
            } else if (cursor == 3) {
                options.linkCommands = !options.linkCommands;
            } else if (cursor == 4) {
                if (!required_files_exist(options)) {
                    options.message = "Source path is not a KSM project.";
                } else if (confirm_install(options)) {
                    return run_installation(terminal, options);
                }
            } else if (cursor == 5) {
                return 0;
            }
        }
    }
}

std::string current_working_directory() {
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) return ".";
    return buffer;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    options.sourcePath = current_working_directory();

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
        if (arg == "--source" && i + 1 < argc) {
            options.sourcePath = argv[++i];
            continue;
        }
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'sudo ./INSTALL.sh --help' to list options.\n";
        return 1;
    }

    return run_tui(options);
}
