#include "main.h"

#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void show_version() {
    std::cout << BLUE << "ksm component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager" << '\n';
    std::cout << "License: MIT" << '\n';
}

void show_help() {
    std::cout << BLUE << "Usage: " << RESET << CYAN << "sudo ksm" << RESET << '\n';
    std::cout << "Opens the Kastiusz System Manager control panel.\n\n";
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  " << CYAN << "--help, -h" << RESET << "       Show this help\n";
    std::cout << "  " << CYAN << "--version, -v" << RESET << "    Show version\n\n";
    std::cout << DIM << "KSM is panel-first now. Everything lives behind sudo ksm." << RESET << '\n';
}

std::string control_path() {
    const std::string installed = "/opt/KSM/bin/kcontrol";
    if (access(installed.c_str(), X_OK) == 0) return installed;
    return "kcontrol";
}

int run_control_panel() {
    if (geteuid() != 0) {
        std::cerr << RED << "Run with sudo:" << RESET << " sudo ksm\n";
        return 1;
    }

    const std::string program = control_path();
    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << RED << "ERROR:" << RESET << " fork failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (pid == 0) {
        execlp(program.c_str(), program.c_str(), nullptr);
        std::cerr << RED << "ERROR:" << RESET << " could not execute " << program << ": " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << RED << "ERROR:" << RESET << " waitpid failed: " << std::strerror(errno) << '\n';
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    bool version = false;
    bool help = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            version = true;
        } else if (arg == "--help" || arg == "-h") {
            help = true;
        } else {
            std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
            std::cerr << "run 'ksm --help' for usage.\n";
            return 1;
        }
    }

    if (version && help) {
        std::cout << CYAN << "--version" << RESET << '\n';
        show_version();
        std::cout << '\n' << CYAN << "--help" << RESET << '\n';
        show_help();
        return 0;
    }
    if (version) {
        show_version();
        return 0;
    }
    if (help) {
        show_help();
        return 0;
    }

    return run_control_panel();
}
