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
    std::cout << BLUE << "Usage: " << RESET << "ksm <command> [options]\n";
    std::cout << BLUE << "Commands:" << RESET << '\n';
    std::cout << "  home            Show KSM help page (khome)\n";
    std::cout << "  help            Show KSM help page (khome)\n";
    std::cout << "  update          Interactive KSM updater (kupgr)\n";
    std::cout << "  upgrade         Interactive KSM updater (kupgr)\n";
    std::cout << "  useradd         Interactive user creator (kuseradd)\n";
    std::cout << "  userdel         Interactive user remover (kuserdel)\n";
    std::cout << '\n';
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  --help, -h      Show this wrapper help\n";
    std::cout << "  --version, -v   Show version information\n";
    std::cout << '\n';
    std::cout << BLUE << "khome pages:" << RESET << '\n';
    std::cout << "  ksm help -p1    Show page 1\n";
    std::cout << "  ksm help -pN    Show page N when it exists\n";
    std::cout << "  ksm help --all  Show all pages\n";
    std::cout << '\n';
    std::cout << BLUE << "updater examples:" << RESET << '\n';
    std::cout << "  ksm upgrade     Update from latest release\n";
    std::cout << "  ksm upgrade -ex Update from latest experimental prerelease\n";
    std::cout << "  ksm upgrade -f  Force reinstall\n";
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

        if (cmd == "update" || cmd == "upgrade") {
            return run_installed_tool("kupgr", forwarded);
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



