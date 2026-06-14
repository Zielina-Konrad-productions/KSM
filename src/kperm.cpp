#include "main.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <pwd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

enum class Key { Up, Down, Left, Right, Tab, Enter, Backspace, Escape, CtrlC, Character, Unknown };

struct KeyPress {
    Key key = Key::Unknown;
    char value = '\0';
};

struct Options {
    std::string path;
    std::string owner;
    std::string group;
    std::string mode;
    bool recursive = false;
    std::string message;
};

struct ProcessResult {
    int exitCode = 1;
};

struct BrowserEntry {
    fs::path path;
    bool directory = false;
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
    std::cout << "               kperm\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kperm component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "kperm [options]\n";
    std::cout << "Interactive chmod/chown helper.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down       Move\n";
    std::cout << "  Left          Go up in file picker\n";
    std::cout << "  Tab           Jump to Apply/Top\n";
    std::cout << "  Enter         Browse path, edit/toggle/action\n";
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

std::string field_value(const std::string& value, const std::string& empty = "(empty)") {
    return value.empty() ? DIM + empty + RESET : value;
}

std::string path_text(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (!ec) return absolute.lexically_normal().string();
    return path.lexically_normal().string();
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

void load_current(Options& options) {
    struct stat st {};
    if (options.path.empty() || stat(options.path.c_str(), &st) != 0) {
        options.message = "Path not found.";
        return;
    }

    std::ostringstream mode;
    mode << std::oct << (st.st_mode & 0777);
    options.mode = mode.str();

    if (struct passwd* pw = getpwuid(st.st_uid)) options.owner = pw->pw_name;
    else options.owner = std::to_string(st.st_uid);

    if (struct group* gr = getgrgid(st.st_gid)) options.group = gr->gr_name;
    else options.group = std::to_string(st.st_gid);

    options.message = "Loaded current permissions.";
}

fs::path picker_start_path(const std::string& current) {
    std::error_code ec;
    if (current.empty()) return fs::current_path(ec);

    fs::path path(current);
    if (fs::is_regular_file(path, ec)) return path.parent_path();
    if (fs::is_directory(path, ec)) return path;

    return fs::current_path(ec);
}

std::vector<BrowserEntry> read_directory(const fs::path& directory) {
    std::vector<BrowserEntry> entries;
    std::error_code ec;

    fs::directory_iterator it(directory, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        BrowserEntry entry;
        entry.path = it->path();
        entry.directory = it->is_directory(ec);
        entries.push_back(entry);
        it.increment(ec);
    }

    std::sort(entries.begin(), entries.end(), [](const BrowserEntry& a, const BrowserEntry& b) {
        if (a.directory != b.directory) return a.directory > b.directory;
        return a.path.filename().string() < b.path.filename().string();
    });
    return entries;
}

void draw_picker_row(int row, int cursor, const std::string& text, bool directory = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    std::cout << (directory ? CYAN : "") << text << RESET << '\n';
}

std::string pick_path(const std::string& current) {
    fs::path directory = picker_start_path(current);
    if (directory.empty()) directory = ".";
    int cursor = 0;
    int offset = 0;
    constexpr int pageSize = 13;

    while (true) {
        const auto entries = read_directory(directory);
        const int maxRow = static_cast<int>(entries.size()) + 1;

        clear_screen();
        banner();
        std::cout << CYAN << "File picker" << RESET << '\n';
        std::cout << CYAN << "Current:" << RESET << " " << path_text(directory) << '\n';
        std::cout << "Up/Down move, Enter select/open, Left goes up, Esc/q cancels.\n\n";

        if (cursor < offset) offset = cursor;
        if (cursor >= offset + pageSize) offset = cursor - pageSize + 1;

        draw_picker_row(0, cursor, "[select this directory]", true);
        draw_picker_row(1, cursor, "[..]", true);

        const int firstEntry = std::max(0, offset - 2);
        const int lastEntry = std::min<int>(static_cast<int>(entries.size()), offset + pageSize - 2);
        for (int i = firstEntry; i < lastEntry; ++i) {
            const int row = i + 2;
            const auto& entry = entries[i];
            const std::string prefix = entry.directory ? "[dir]  " : "[file] ";
            draw_picker_row(row, cursor, prefix + entry.path.filename().string(), entry.directory);
        }

        if (entries.empty()) std::cout << DIM << "\n  (empty directory)" << RESET << '\n';
        std::cout << std::flush;

        const KeyPress key = read_key();
        if (key.key == Key::Up) {
            cursor = std::max(0, cursor - 1);
        } else if (key.key == Key::Down) {
            cursor = std::min(maxRow, cursor + 1);
        } else if (key.key == Key::Left) {
            directory = directory.parent_path().empty() ? directory : directory.parent_path();
            cursor = 0;
            offset = 0;
        } else if (key.key == Key::Enter) {
            if (cursor == 0) return path_text(directory);
            if (cursor == 1) {
                directory = directory.parent_path().empty() ? directory : directory.parent_path();
                cursor = 0;
                offset = 0;
                continue;
            }

            const int index = cursor - 2;
            if (index >= 0 && index < static_cast<int>(entries.size())) {
                const auto& entry = entries[index];
                if (entry.directory) {
                    directory = entry.path;
                    cursor = 0;
                    offset = 0;
                } else {
                    return path_text(entry.path);
                }
            }
        } else if (key.key == Key::Escape || key.key == Key::CtrlC ||
                   (key.key == Key::Character && key.value == 'q')) {
            return current;
        }
    }
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
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " apply/top  "
              << CYAN << "Enter:" << RESET << " browse/edit/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    draw_field(0, cursor, "Path", field_value(options.path));
    draw_field(1, cursor, "Owner", field_value(options.owner, "(unchanged)"));
    draw_field(2, cursor, "Group", field_value(options.group, "(unchanged)"));
    draw_field(3, cursor, "Mode", field_value(options.mode, "(unchanged)"));
    draw_field(4, cursor, "Recursive", yes_no(options.recursive));
    draw_field(5, cursor, "Load current", "Enter");
    draw_field(6, cursor, "Apply changes", GREEN + "Enter" + RESET);
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
        } else if (key.key == Key::Character) {
            value.push_back(key.value);
        }
    }
}

bool valid_mode(const std::string& mode) {
    if (mode.empty()) return true;
    if (mode.size() < 3 || mode.size() > 4) return false;
    return std::all_of(mode.begin(), mode.end(), [](unsigned char ch) {
        return ch >= '0' && ch <= '7';
    });
}

bool validate(Options& options) {
    if (options.path.empty()) {
        options.message = "Path is required.";
        return false;
    }
    if (!valid_mode(options.mode)) {
        options.message = "Mode must be octal, for example 755.";
        return false;
    }
    if (options.owner.empty() && options.group.empty() && options.mode.empty()) {
        options.message = "Nothing to apply.";
        return false;
    }
    options.message.clear();
    return true;
}

bool confirm_apply(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to update permissions" << RESET << "\n\n";
    std::cout << "Path      : " << options.path << '\n';
    std::cout << "Owner     : " << (options.owner.empty() ? "-" : options.owner) << '\n';
    std::cout << "Group     : " << (options.group.empty() ? "-" : options.group) << '\n';
    std::cout << "Mode      : " << (options.mode.empty() ? "-" : options.mode) << '\n';
    std::cout << "Recursive : " << (options.recursive ? "yes" : "no") << "\n\n";
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to apply, any other key to return.\n";
    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

int apply_changes(const Options& options) {
    clear_screen();
    banner();
    int failures = 0;

    if (!options.owner.empty() || !options.group.empty()) {
        std::vector<std::string> args = {"chown"};
        if (options.recursive) args.push_back("-R");
        if (!options.owner.empty() && !options.group.empty()) {
            args.push_back(options.owner + ":" + options.group);
        } else if (!options.owner.empty()) {
            args.push_back(options.owner);
        } else {
            args.push_back(":" + options.group);
        }
        args.push_back(options.path);
        std::cout << CYAN << "[*]" << RESET << " Running chown...\n";
        const int code = run_process(args).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Owner/group updated.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " chown failed (exit " << code << ")\n";
        }
    }

    if (!options.mode.empty()) {
        std::vector<std::string> args = {"chmod"};
        if (options.recursive) args.push_back("-R");
        args.push_back(options.mode);
        args.push_back(options.path);
        std::cout << CYAN << "[*]" << RESET << " Running chmod...\n";
        const int code = run_process(args).exitCode;
        if (code == 0) std::cout << GREEN << "[+]" << RESET << " Mode updated.\n";
        else {
            ++failures;
            std::cout << RED << "[x]" << RESET << " chmod failed (exit " << code << ")\n";
        }
    }

    std::cout << '\n' << (failures == 0 ? GREEN + "Done." : RED + "Done with failures: " + std::to_string(failures))
              << RESET << '\n';
    std::cout << "Press any key to exit.\n";
    read_key();
    return failures == 0 ? 0 : 1;
}

void move_cursor(int& cursor, int delta) {
    constexpr int maxRow = 7;
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

    Terminal terminal;
    int cursor = 0;

    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();
        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) move_cursor(cursor, -1);
        else if (key.key == Key::Down) move_cursor(cursor, 1);
        else if (key.key == Key::Tab) cursor = cursor < 6 ? 6 : 0;
        else if (key.key == Key::Left || key.key == Key::Right) {
            if (cursor == 4) options.recursive = !options.recursive;
        } else if (key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) {
                options.path = pick_path(options.path);
                if (!options.path.empty()) load_current(options);
            }
            else if (cursor == 1) options.owner = edit_value("Owner", options.owner);
            else if (cursor == 2) options.group = edit_value("Group", options.group);
            else if (cursor == 3) options.mode = edit_value("Mode", options.mode);
            else if (cursor == 4) options.recursive = !options.recursive;
            else if (cursor == 5) load_current(options);
            else if (cursor == 6) {
                if (validate(options) && confirm_apply(options)) return apply_changes(options);
            } else if (cursor == 7) return 0;
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
        if (options.path.empty()) {
            options.path = arg;
            continue;
        }
        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kperm --help' to list options.\n";
        return 1;
    }
    if (options.path.empty()) {
        std::error_code ec;
        options.path = fs::current_path(ec).string();
    }
    if (!options.path.empty()) load_current(options);
    return run_tui(options);
}
