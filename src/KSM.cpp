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
    std::cout << BLUE << "Usage: " << RESET << CYAN << "ksm" << RESET << " <command> [options]\n";
    std::cout << BLUE << "Main tools:" << RESET << '\n';
    std::cout << "  " << CYAN << "khome" << RESET << "             Home/help browser\n";
    std::cout << "  " << CYAN << "kupgr" << RESET << "             GitHub Releases updater\n";
    std::cout << "  " << CYAN << "kuninstall" << RESET << "        Uninstaller\n";
    std::cout << "  " << CYAN << "kgroupadd" << RESET << "         Group creator\n";
    std::cout << "  " << CYAN << "kgroupdel" << RESET << "         Group remover\n";
    std::cout << "  " << CYAN << "kuseradd" << RESET << "          User creator\n";
    std::cout << "  " << CYAN << "kuserdel" << RESET << "          User remover\n";
    std::cout << "  " << CYAN << "knetcfg" << RESET << "           Network interface config\n";
    std::cout << '\n';
    std::cout << BLUE << "KSM alternatives:" << RESET << '\n';
    std::cout << "  " << CYAN << "ksm home" << RESET << "          Same as khome\n";
    std::cout << "  " << CYAN << "ksm upgrade" << RESET << "       Same as kupgr\n";
    std::cout << "  " << CYAN << "ksm uninstall" << RESET << "     Same as kuninstall\n";
    std::cout << "  " << CYAN << "ksm groupadd" << RESET << "      Same as kgroupadd\n";
    std::cout << "  " << CYAN << "ksm groupdel" << RESET << "      Same as kgroupdel\n";
    std::cout << "  " << CYAN << "ksm useradd" << RESET << "       Same as kuseradd\n";
    std::cout << "  " << CYAN << "ksm userdel" << RESET << "       Same as kuserdel\n";
    std::cout << "  " << CYAN << "ksm netcfg" << RESET << "        Same as knetcfg\n";
    std::cout << '\n';
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  " << CYAN << "--help, -h" << RESET << "      Show this wrapper help\n";
    std::cout << "  " << CYAN << "--version, -v" << RESET << "   Show version information\n";
    std::cout << '\n';
    std::cout << BLUE << "khome pages:" << RESET << '\n';
    std::cout << "  " << CYAN << "ksm home -p1" << RESET << "    Show page 1\n";
    std::cout << "  " << CYAN << "ksm home -pN" << RESET << "    Show page N when it exists\n";
    std::cout << "  " << CYAN << "ksm home --all" << RESET << "  Show all pages\n";
    std::cout << '\n';
    std::cout << BLUE << "updater examples:" << RESET << '\n';
    std::cout << "  " << CYAN << "ksm upgrade" << RESET << "     Update from latest release\n";
    std::cout << "  " << CYAN << "ksm upgrade -ex" << RESET << " Update from latest experimental prerelease\n";
    std::cout << "  " << CYAN << "ksm upgrade -f" << RESET << "  Force reinstall\n";
}

int run_program(const std::string& program, const std::vector<std::string>& args) {
    std::vector<char*> exec_args;
    exec_args.push_back(const_cast<char*>(program.c_str()));

    for (const auto& arg : args) {
        exec_args.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << RED << "ERROR:" << RESET << " fork failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (pid == 0) {
        execvp(program.c_str(), exec_args.data());
        std::cerr << RED << "ERROR:" << RESET << " could not execute " << program << ": " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << RED << "ERROR:" << RESET << " waitpid failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int run_installed_tool(const std::string& tool_name, const std::vector<std::string>& args) {
    const std::string installed_path = "/opt/KSM/bin/" + tool_name;
    if (access(installed_path.c_str(), X_OK) == 0) {
        return run_program(installed_path, args);
    }
    return run_program(tool_name, args);
}

} // namespace

int main(int argc, char* argv[]) {
    bool version = false;
    bool help = false;
    int cmd_index = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            version = true;
        } else if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (cmd_index == -1) {
            cmd_index = i;
        }
    }

    if (cmd_index != -1) {
        const std::string cmd = argv[cmd_index];
        std::vector<std::string> forwarded;
        for (int i = 1; i < argc; ++i) {
            if (i != cmd_index) {
                forwarded.emplace_back(argv[i]);
            }
        }

        if (cmd == "home" || cmd == "help") {
            return run_installed_tool("khome", forwarded);
        }

        if (cmd == "useradd") {
            return run_installed_tool("kuseradd", forwarded);
        }

        if (cmd == "userdel") {
            return run_installed_tool("kuserdel", forwarded);
        }

        if (cmd == "groupadd") {
            return run_installed_tool("kgroupadd", forwarded);
        }

        if (cmd == "groupdel") {
            return run_installed_tool("kgroupdel", forwarded);
        }

        if (cmd == "netcfg" || cmd == "network") {
            return run_installed_tool("knetcfg", forwarded);
        }

        if (cmd == "upgrade") {
            return run_installed_tool("kupgr", forwarded);
        }

        if (cmd == "uninstall") {
            return run_installed_tool("kuninstall", forwarded);
        }

        std::cerr << RED << "unknown command:" << RESET << " " << cmd << '\n';
        std::cerr << "run 'ksm --help' to list commands.\n";
        return 1;
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

    return run_installed_tool("khome", {});
}



