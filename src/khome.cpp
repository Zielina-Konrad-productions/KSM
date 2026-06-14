#include "main.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

namespace {

constexpr const char* kConfigPath = "/opt/KSM/kastiusz.conf";

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
    std::cout << '\n';
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  " << CYAN << "--edit-config, -ed" << RESET << "  Edit "
              << CYAN << "/opt/KSM/kastiusz.conf" << RESET << '\n';
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
    std::cout << "  " << CYAN << "kuseradd" << RESET << "    - User creator"
              << DIM << " | alt: ksm useradd" << RESET << '\n';
    std::cout << "  " << CYAN << "kuserdel" << RESET << "    - User remover"
              << DIM << " | alt: ksm userdel" << RESET << '\n';
    std::cout << "  " << CYAN << "kgroupadd" << RESET << "   - Group creator"
              << DIM << " | alt: ksm groupadd" << RESET << '\n';
    std::cout << "  " << CYAN << "kgroupdel" << RESET << "   - Group remover"
              << DIM << " | alt: ksm groupdel" << RESET << '\n';
    std::cout << "  " << CYAN << "knetcfg" << RESET << "     - Network interface config"
              << DIM << " | alt: ksm netcfg" << RESET << "\n\n";

    std::cout << BOLD << BLUE << "Examples:" << RESET << '\n';
    std::cout << "  " << CYAN << "khome -p1" << RESET << "      - Show page 1\n";
    std::cout << "  " << CYAN << "khome --all" << RESET << "    - Show all pages\n";
    std::cout << "  " << CYAN << "kupgr -ex" << RESET << "     - Use latest experimental prerelease\n";
    std::cout << "  " << CYAN << "sudo knetcfg" << RESET << "  - Configure a network interface\n";
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
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    if (access("nano", X_OK) != 0 && system("command -v nano >/dev/null 2>&1") != 0) {
        std::cerr << RED << "ERROR:" << RESET << " install nano first.\n";
        return 1;
    }

    execlp("nano", "nano", kConfigPath, nullptr);
    std::cerr << RED << "ERROR:" << RESET << " could not start nano: " << std::strerror(errno) << '\n';
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<int> selected_pages;
    bool selected_all = false;
    bool selected_help = false;
    bool selected_version = false;
    bool selected_config = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            selected_version = true;
        } else if (arg == "--help" || arg == "-h") {
            selected_help = true;
        } else if (arg == "--all" || arg == "-a") {
            selected_all = true;
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
