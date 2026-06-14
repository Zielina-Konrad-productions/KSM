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
    std::cout << BLUE << "Commands:" << RESET << '\n';
    std::cout << "  " << CYAN << "home" << RESET << "       Full KSM home pages\n";
    std::cout << "  " << CYAN << "upgrade" << RESET << "    Update KSM\n";
    std::cout << "  " << CYAN << "uninstall" << RESET << "  Uninstall KSM\n";
    std::cout << "  " << CYAN << "sysinfo" << RESET << "    System dashboard\n";
    std::cout << "  " << CYAN << "serv" << RESET << "       Manage systemd services\n";
    std::cout << "  " << CYAN << "perm" << RESET << "       Manage file permissions\n";
    std::cout << "  " << CYAN << "ssh" << RESET << "        Configure SSH daemon\n";
    std::cout << "  " << CYAN << "firewall" << RESET << "   Manage firewall rules\n";
    std::cout << "  " << CYAN << "useradd" << RESET << "    Add users\n";
    std::cout << "  " << CYAN << "usermod" << RESET << "    Modify users\n";
    std::cout << "  " << CYAN << "userdel" << RESET << "    Delete users\n";
    std::cout << "  " << CYAN << "groupadd" << RESET << "   Add groups\n";
    std::cout << "  " << CYAN << "groupmod" << RESET << "   Modify groups\n";
    std::cout << "  " << CYAN << "groupdel" << RESET << "   Delete groups\n";
    std::cout << "  " << CYAN << "netcfg" << RESET << "     Configure network interface\n";
    std::cout << '\n';
    std::cout << BLUE << "Options:" << RESET << " "
              << CYAN << "--help/-h" << RESET << ", "
              << CYAN << "--version/-v" << RESET << '\n';
    std::cout << DIM << "Full tool list: khome -p2 or ksm home -p2" << RESET << '\n';
    std::cout << DIM << "Interactive home: khome -ui or ksm home -ui" << RESET << '\n';
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

        if (cmd == "usermod") {
            return run_installed_tool("kusermod", forwarded);
        }

        if (cmd == "groupadd") {
            return run_installed_tool("kgroupadd", forwarded);
        }

        if (cmd == "groupmod") {
            return run_installed_tool("kgroupmod", forwarded);
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

        if (cmd == "sysinfo" || cmd == "info") {
            return run_installed_tool("ksysinfo", forwarded);
        }

        if (cmd == "serv" || cmd == "service" || cmd == "services") {
            return run_installed_tool("kserv", forwarded);
        }

        if (cmd == "perm" || cmd == "permissions") {
            return run_installed_tool("kperm", forwarded);
        }

        if (cmd == "ssh") {
            return run_installed_tool("kssh", forwarded);
        }

        if (cmd == "firewall" || cmd == "fw") {
            return run_installed_tool("kfirewall", forwarded);
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

    show_help();
    return 0;
}



