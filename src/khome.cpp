#include "main.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr const char* kConfigPath = "/opt/KSM/kastiusz.conf";

enum class Key {
    Up,
    Down,
    Left,
    Right,
    Tab,
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

struct ConfigOptions {
    int defaultPage = 1;
    bool showAllPages = false;
    std::string message;
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
    std::cout << "\033[2J\033[H";
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

void show_version() {
    std::cout << BLUE << "khome component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager" << '\n';
    std::cout << "License: MIT" << '\n';
}

void show_help() {
    std::cout << BLUE << "Usage: " << RESET << CYAN << "khome" << RESET << " [options]\n";
    std::cout << BLUE << "Pages:" << RESET << '\n';
    std::cout << "  " << CYAN << "-p1" << RESET << "             Show page 1\n";
    std::cout << "  " << CYAN << "-pN" << RESET << "             Show page N when it exists\n";
    std::cout << "  " << CYAN << "--all, -a" << RESET << "       Show all pages\n";
    std::cout << "  " << CYAN << "--ui, -ui" << RESET << "       Interactive khome\n";
    std::cout << '\n';
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  " << CYAN << "--edit-config, -ed" << RESET << "  Edit "
              << CYAN << "/opt/KSM/kastiusz.conf" << RESET << " in KSM UI\n";
    std::cout << "  " << CYAN << "--help, -h" << RESET << "          Show this help\n";
    std::cout << "  " << CYAN << "--version, -v" << RESET << "       Show version information\n";
}

void show_banner() {
    std::cout << BLUE << '\n';
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "                 Help\n";
    std::cout << "========================================\n";
    std::cout << RESET;
    std::cout << "Version: " << CYAN << "v" << ksm_version::version() << RESET << "\n\n";
}

void show_page_info() {
    std::cout << CYAN << "PAGE Information:" << RESET << '\n';
    std::cout << BOLD << "PAGE 1" << RESET << " - KSM overview and configuration\n";
    std::cout << BOLD << "PAGE 2" << RESET << " - KSM tools and wrapper alternatives\n\n";
}

void page1() {
    std::cout << CYAN << "Current PAGE:" << RESET << '\n';
    std::cout << BOLD << "PAGE 1" << RESET << " (KSM overview and configuration)\n\n";

    std::cout << BOLD << BLUE << "KSM Information:" << RESET << '\n';
    std::cout << "  Name: " << CYAN << "Kastiusz System Manager" << RESET << '\n';
    std::cout << "  Version: " << CYAN << "v" << ksm_version::version() << RESET << '\n';
    std::cout << "  Install path: " << CYAN << "/opt/KSM" << RESET << '\n';
    std::cout << "  Config file: " << CYAN << "/opt/KSM/kastiusz.conf" << RESET << "\n\n";

    std::cout << BOLD << BLUE << "khome configuration:" << RESET << '\n';
    std::cout << "  " << CYAN << "khome-default-page-1=true" << RESET << '\n';
    std::cout << "  " << CYAN << "khome-show-all-pages=false" << RESET << "\n\n";

    std::cout << BOLD << "Open " << CYAN << "khome -p2" << RESET << BOLD
              << " for tools and command alternatives." << RESET << '\n';
}

void page2() {
    std::cout << CYAN << "Current PAGE:" << RESET << '\n';
    std::cout << BOLD << "PAGE 2" << RESET << " (KSM tools and wrapper alternatives)\n\n";

    std::cout << BOLD << BLUE << "Main tools:" << RESET << '\n';
    std::cout << "  " << CYAN << "khome" << RESET << "       - Home/help browser"
              << DIM << " | alt: ksm home" << RESET << '\n';
    std::cout << "  " << CYAN << "kupgr" << RESET << "       - GitHub Releases updater"
              << DIM << " | alt: ksm upgrade" << RESET << '\n';
    std::cout << "  " << CYAN << "kuninstall" << RESET << " - Uninstaller"
              << DIM << " | alt: ksm uninstall" << RESET << '\n';
    std::cout << "  " << CYAN << "ksysinfo" << RESET << "   - System dashboard"
              << DIM << " | alt: ksm sysinfo" << RESET << '\n';
    std::cout << "  " << CYAN << "kserv" << RESET << "      - Systemd service manager"
              << DIM << " | alt: ksm serv" << RESET << '\n';
    std::cout << "  " << CYAN << "kperm" << RESET << "      - File permission manager"
              << DIM << " | alt: ksm perm" << RESET << '\n';
    std::cout << "  " << CYAN << "kssh" << RESET << "       - SSH daemon config"
              << DIM << " | alt: ksm ssh" << RESET << '\n';
    std::cout << "  " << CYAN << "kfirewall" << RESET << "  - Firewall helper"
              << DIM << " | alt: ksm firewall" << RESET << '\n';
    std::cout << "  " << CYAN << "kuseradd" << RESET << "    - User creator"
              << DIM << " | alt: ksm useradd" << RESET << '\n';
    std::cout << "  " << CYAN << "kusermod" << RESET << "    - User modifier"
              << DIM << " | alt: ksm usermod" << RESET << '\n';
    std::cout << "  " << CYAN << "kuserdel" << RESET << "    - User remover"
              << DIM << " | alt: ksm userdel" << RESET << '\n';
    std::cout << "  " << CYAN << "kgroupadd" << RESET << "   - Group creator"
              << DIM << " | alt: ksm groupadd" << RESET << '\n';
    std::cout << "  " << CYAN << "kgroupmod" << RESET << "   - Group modifier"
              << DIM << " | alt: ksm groupmod" << RESET << '\n';
    std::cout << "  " << CYAN << "kgroupdel" << RESET << "   - Group remover"
              << DIM << " | alt: ksm groupdel" << RESET << '\n';
    std::cout << "  " << CYAN << "knetcfg" << RESET << "     - Network interface config"
              << DIM << " | alt: ksm netcfg" << RESET << "\n\n";

    std::cout << BOLD << BLUE << "Examples:" << RESET << '\n';
    std::cout << "  " << CYAN << "khome -p1" << RESET << "      - Show page 1\n";
    std::cout << "  " << CYAN << "khome --all" << RESET << "    - Show all pages\n";
    std::cout << "  " << CYAN << "kupgr -ex" << RESET << "     - Use latest experimental prerelease\n";
    std::cout << "  " << CYAN << "sudo knetcfg" << RESET << "  - Configure a network interface\n";
    std::cout << "  " << CYAN << "sudo kssh" << RESET << "    - Configure SSH daemon\n";
}

void page_unavailable(int page) {
    std::cout << CYAN << "Current PAGE:" << RESET << '\n';
    std::cout << BOLD << "PAGE " << page << RESET << '\n';
    std::cout << YELLOW << "This page is not added yet." << RESET << '\n';
}

void all_pages() {
    page1();
    std::cout << '\n';
    page2();
}

void show_page(int page) {
    if (page == 1) {
        page1();
        return;
    }
    if (page == 2) {
        page2();
        return;
    }
    page_unavailable(page);
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

void move_cursor(int& cursor, int delta, int maxRow) {
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

ConfigOptions load_config_options() {
    ConfigOptions options;
    options.defaultPage = khome_config::defaultpage(2) ? 2 : 1;
    options.showAllPages = khome_config::showallpages();
    return options;
}

bool save_config_options(const ConfigOptions& options) {
    std::ofstream conf(kConfigPath, std::ios::trunc);
    if (!conf.is_open()) return false;

    conf << "# ---------------------------------------------------------------------------\n";
    conf << "# Kastiusz System Manager Configuration\n";
    conf << "# Location after install: /opt/KSM/kastiusz.conf\n";
    conf << "# ---------------------------------------------------------------------------\n\n";
    conf << "[UI-khome]\n";
    conf << "# Default page shown by khome when no page option is passed.\n";
    conf << "# Exactly one default page should normally be true, unless show-all is true.\n\n";
    conf << "# PAGE 1 - KSM overview and configuration information\n";
    conf << "khome-default-page-1=" << (options.defaultPage == 1 ? "true" : "false") << "\n\n";
    conf << "# PAGE 2 - KSM tools and wrapper alternatives\n";
    conf << "khome-default-page-2=" << (options.defaultPage == 2 ? "true" : "false") << "\n\n";
    conf << "# Show all khome pages at once.\n";
    conf << "# If true, default page options are ignored.\n";
    conf << "khome-show-all-pages=" << (options.showAllPages ? "true" : "false") << '\n';
    return conf.good();
}

void draw_config_editor(const ConfigOptions& options, int cursor) {
    clear_screen();
    show_banner();
    std::cout << CYAN << "Interactive config editor" << RESET << "\n\n";
    std::cout << "Config file: " << CYAN << kConfigPath << RESET << "\n\n";

    draw_row(0, cursor, "Default page          PAGE " + std::to_string(options.defaultPage));
    draw_row(1, cursor, "Show all pages        " + yes_no(options.showAllPages));
    draw_row(2, cursor, "Save config           Enter", true);
    draw_row(3, cursor, "Cancel                Enter or q");

    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
    std::cout << std::flush;
}

int edit_config();

void wait_for_key() {
    std::cout << "\n" << DIM << "Press any key to return." << RESET << std::flush;
    read_key();
}

void show_page_screen(int page) {
    clear_screen();
    show_banner();
    show_page_info();
    show_page(page);
    wait_for_key();
}

void show_all_pages_screen() {
    clear_screen();
    show_banner();
    show_page_info();
    all_pages();
    wait_for_key();
}

void draw_interactive_home(int cursor) {
    clear_screen();
    show_banner();
    std::cout << CYAN << "Interactive khome" << RESET << "\n\n";

    draw_row(0, cursor, "Open PAGE 1           Enter");
    draw_row(1, cursor, "Open PAGE 2           Enter");
    draw_row(2, cursor, "Show all pages        Enter");
    draw_row(3, cursor, "Edit config           Enter");
    draw_row(4, cursor, "Exit                  Enter or q");

    std::cout << '\n' << DIM << "Use arrows, Tab, Enter, q." << RESET << std::flush;
}

int interactive_home() {
    Terminal terminal;
    int cursor = 0;

    while (true) {
        draw_interactive_home(cursor);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, 4);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, 4);
        } else if (key.key == Key::Tab) {
            cursor = cursor < 3 ? 3 : 0;
        } else if (key.key == Key::Enter) {
            if (cursor == 0) {
                show_page_screen(1);
            } else if (cursor == 1) {
                show_page_screen(2);
            } else if (cursor == 2) {
                show_all_pages_screen();
            } else if (cursor == 3) {
                terminal.restore();
                return edit_config();
            } else if (cursor == 4) {
                return 0;
            }
        }
    }
    return 0;
}

bool parse_page_arg(const std::string& arg, int& page) {
    if (arg.rfind("-p", 0) != 0 || arg.size() <= 2) {
        return false;
    }

    try {
        page = std::stoi(arg.substr(2));
    } catch (const std::exception&) {
        return false;
    }

    return page > 0;
}

int edit_config() {
    if (geteuid() != 0) {
        execlp("sudo", "sudo", "khome", "--edit-config", nullptr);
        std::cerr << RED << "ERROR:" << RESET << " could not start sudo: " << std::strerror(errno) << '\n';
        return 1;
    }

    Terminal terminal;
    ConfigOptions options = load_config_options();
    int cursor = 0;

    while (true) {
        draw_config_editor(options, cursor);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) return 0;
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, 3);
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, 3);
        } else if (key.key == Key::Tab) {
            cursor = cursor < 2 ? 2 : 0;
        } else if (key.key == Key::Left || key.key == Key::Right || key.key == Key::Enter) {
            options.message.clear();
            if (cursor == 0) {
                options.defaultPage = options.defaultPage == 1 ? 2 : 1;
            } else if (cursor == 1) {
                options.showAllPages = !options.showAllPages;
            } else if (cursor == 2) {
                options.message = save_config_options(options) ? "Config saved." : "Could not write config file.";
            } else if (cursor == 3) {
                return 0;
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<int> selected_pages;
    bool selected_all = false;
    bool selected_help = false;
    bool selected_version = false;
    bool selected_config = false;
    bool selected_ui = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            selected_version = true;
        } else if (arg == "--help" || arg == "-h") {
            selected_help = true;
        } else if (arg == "--all" || arg == "-a") {
            selected_all = true;
        } else if (arg == "--ui" || arg == "-ui") {
            selected_ui = true;
        } else if (arg == "--edit-config" || arg == "-ed") {
            selected_config = true;
        } else {
            int page = 0;
            if (parse_page_arg(arg, page)) {
                selected_pages.push_back(page);
            } else {
                std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
                std::cerr << "run 'khome --help' to list options.\n";
                return 1;
            }
        }
    }

    if (selected_config && (argc > 2)) {
        std::cerr << RED << "ERROR:" << RESET << " --edit-config / -ed must be used alone\n";
        return 1;
    }

    if (selected_ui && argc > 2) {
        std::cerr << RED << "ERROR:" << RESET << " --ui / -ui must be used alone\n";
        return 1;
    }

    if (selected_version && selected_help) {
        std::cout << CYAN << "--version" << RESET << '\n';
        show_version();
        std::cout << '\n' << CYAN << "--help" << RESET << '\n';
        show_help();
        return 0;
    }

    if (selected_version) {
        show_version();
        return 0;
    }

    if (selected_help) {
        show_help();
        return 0;
    }

    if (selected_config) {
        return edit_config();
    }

    if (selected_ui) {
        return interactive_home();
    }

    show_banner();
    show_page_info();

    if (selected_all) {
        all_pages();
        return 0;
    }

    if (!selected_pages.empty()) {
        for (size_t i = 0; i < selected_pages.size(); ++i) {
            if (i > 0) {
                std::cout << '\n';
            }
            show_page(selected_pages[i]);
        }
        return 0;
    }

    if (khome_config::showallpages()) {
        all_pages();
        return 0;
    }

    if (khome_config::defaultpage(2)) {
        show_page(2);
        return 0;
    }

    if (khome_config::defaultpage(1)) {
        page1();
        return 0;
    }

    page1();
    return 0;
}
