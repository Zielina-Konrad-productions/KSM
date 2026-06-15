#include "main.h"

#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <grp.h>
#include <mutex>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace ftxui;

enum class ModuleKind {
    Home,
    SysInfo,
    Upgrade,
    UserAdd,
    UserMod,
    UserDel,
    GroupAdd,
    GroupMod,
    GroupDel,
    Services,
    Permissions,
    Network,
    Power,
    SSH,
    Firewall,
    Samba,
    Apache,
    DnsServer,
    DhcpServer,
    FtpServer,
    ExtensionInstall,
    ZpmExtension,
    Uninstall
};

enum class FocusPane { Categories, Modules };
enum class PanelFocus { List, Rows };
enum class ViewMode { Menu, NativePanel };
enum class RowType { Text, Bool, Choice, Action, Info };
enum class PickerKind { None, Groups, Users };

struct Module {
    std::string title;
    std::string tool;
    std::vector<std::string> args;
    std::string description;
    bool needsRoot = false;
    ModuleKind kind = ModuleKind::Home;
};

struct Category {
    std::string title;
    std::vector<Module> modules;
};

struct Row {
    RowType type = RowType::Text;
    std::string id;
    std::string label;
    std::string value;
    std::vector<std::string> choices;
    bool checked = false;
    bool danger = false;
    std::string description;
};

struct ListItem {
    std::string key;
    std::string label;
    std::string detail;
    bool selected = false;
    bool directory = false;
    fs::path path;
};

struct NativePanel {
    ModuleKind kind = ModuleKind::Home;
    std::string title;
    std::string subtitle;
    std::string selectedKey;
    std::string message;
    std::string armedAction;
    std::string noticeTitle;
    std::string noticeBody;
    bool noticeOk = false;
    std::vector<Row> rows;
    std::vector<ListItem> list;
    std::vector<std::string> detailLines;
    int rowIndex = 0;
    int listIndex = 0;
    PanelFocus focus = PanelFocus::Rows;
    bool multiList = false;
    fs::path directory;
};

struct TextEdit {
    bool active = false;
    std::string rowId;
    std::string title;
    std::string buffer;
};

struct PickerOverlay {
    bool active = false;
    PickerKind kind = PickerKind::None;
    std::string rowId;
    std::string title;
    std::vector<ListItem> items;
    int index = 0;
};

struct CommandResult {
    int code = 1;
    std::string output;
};

struct ProgressState {
    std::mutex mutex;
    std::vector<std::string> steps = {
        "Authorize",
        "Check tools",
        "Fetch release",
        "Download",
        "Extract",
        "Write version",
        "Build",
        "Stage install",
        "Activate",
        "Finish"
    };
    std::vector<std::string> logLines;
    int currentStep = 0;
    bool done = false;
    bool ok = false;
    int exitCode = 1;
    bool upToDate = false;
    bool completed = false;
    std::string title = "KSM UPDATE";
    std::string status = "Preparing update...";
    std::string output;
};

void update_progress_from_line(ProgressState& state, const std::string& line);
Element render_progress(const ProgressState& state);
bool command_exists(const std::string& command);
bool zpm_installed();

void version() {
    std::cout << BLUE << "kcontrol component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "sudo ksm\n";
    std::cout << "Internal FTXUI control center helper.\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Tab/Left/Right Switch focus between panels\n";
    std::cout << "  Up/Down        Move in focused panel\n";
    std::cout << "  Enter          Select, edit, toggle, or run action\n";
    std::cout << "  F1/?           Help overlay\n";
    std::cout << "  q/Esc          Back or exit\n";
}

std::vector<Category> categories() {
    std::vector<Category> items = {
        {
            "Overview",
            {
                {"KSM Home", "khome", {}, "Browse KSM pages inside the control center", false, ModuleKind::Home},
                {"System Info", "ksysinfo", {}, "Live system dashboard", false, ModuleKind::SysInfo}
            }
        },
        {
            "Users",
            {
                {"Add Users", "kuseradd", {}, "Create users", true, ModuleKind::UserAdd},
                {"Modify Users", "kusermod", {}, "Edit user account settings", true, ModuleKind::UserMod},
                {"Delete Users", "kuserdel", {}, "Remove user accounts", true, ModuleKind::UserDel},
                {"Add Groups", "kgroupadd", {}, "Create groups", true, ModuleKind::GroupAdd},
                {"Modify Groups", "kgroupmod", {}, "Edit groups and members", true, ModuleKind::GroupMod},
                {"Delete Groups", "kgroupdel", {}, "Remove groups", true, ModuleKind::GroupDel}
            }
        },
        {
            "System",
            {
                {"Permissions", "kperm", {}, "Browse files and edit chmod/chown", true, ModuleKind::Permissions},
                {"Network Config", "knetcfg", {}, "Configure network interfaces", true, ModuleKind::Network}
            }
        },
        {
            "Services",
            {
                {"Systemd Services", "kserv", {}, "Start, stop and enable systemd services", false, ModuleKind::Services},
                {"Samba", "samba", {}, "Install and configure Samba shares", true, ModuleKind::Samba},
                {"Apache", "apache", {}, "Install and configure Apache virtual host", true, ModuleKind::Apache},
                {"DNS", "dns", {}, "Install and configure a simple BIND DNS cache", true, ModuleKind::DnsServer},
                {"DHCP", "dhcp", {}, "Install and configure a DHCP server", true, ModuleKind::DhcpServer},
                {"FTP", "ftp", {}, "Install and configure vsftpd", true, ModuleKind::FtpServer},
                {"Power", "power", {}, "Shutdown, reboot, and cancel scheduled shutdowns", true, ModuleKind::Power}
            }
        },
        {
            "Security",
            {
                {"SSH Config", "kssh", {}, "Configure sshd safely with backup", true, ModuleKind::SSH},
                {"Firewall", "kfirewall", {}, "Manage ufw or firewalld rules", true, ModuleKind::Firewall}
            }
        },
        {
            "Maintenance",
            {
                {"Uninstall KSM", "kuninstall", {}, "Remove KSM installation", true, ModuleKind::Uninstall},
                {"Updater", "kupgr", {}, "Stable, prerelease and snapshot updater", true, ModuleKind::Upgrade}
            }
        },
        {
            "Extensions",
            {
                {"ZPM", "zpm", {}, "Install ZPM extension from GitHub", true, ModuleKind::ExtensionInstall}
            }
        }
    };

    if (zpm_installed()) {
        items.push_back({
            "ZPM",
            {
                {"Functions", "zpm", {}, "Manage ZPM functions inside KSM", true, ModuleKind::ZpmExtension}
            }
        });
    }

    return items;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ",";
        result += value;
    }
    return result;
}

bool contains_value(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string replace_index(std::string value, int index) {
    const std::string marker = "$i";
    const std::string replacement = std::to_string(index);
    size_t pos = 0;
    while ((pos = value.find(marker, pos)) != std::string::npos) {
        value.replace(pos, marker.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::string fit_text(const std::string& value, size_t width) {
    if (value.size() <= width) return value;
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::string command_text(const Module& module) {
    std::string result = module.tool;
    for (const auto& arg : module.args) result += " " + arg;
    return result;
}

Row* find_row(NativePanel& panel, const std::string& id) {
    for (auto& row : panel.rows) {
        if (row.id == id) return &row;
    }
    return nullptr;
}

const Row* find_row(const NativePanel& panel, const std::string& id) {
    for (const auto& row : panel.rows) {
        if (row.id == id) return &row;
    }
    return nullptr;
}

std::string row_value(const NativePanel& panel, const std::string& id) {
    const Row* row = find_row(panel, id);
    return row ? row->value : "";
}

bool row_checked(const NativePanel& panel, const std::string& id) {
    const Row* row = find_row(panel, id);
    return row && row->checked;
}

void set_row_value(NativePanel& panel, const std::string& id, const std::string& value) {
    if (Row* row = find_row(panel, id)) row->value = value;
}

void set_row_checked(NativePanel& panel, const std::string& id, bool value) {
    if (Row* row = find_row(panel, id)) row->checked = value;
}

std::vector<std::string> selected_keys(const NativePanel& panel) {
    std::vector<std::string> result;
    for (const auto& item : panel.list) {
        if (item.selected) result.push_back(item.key);
    }
    return result;
}

int parse_positive(const std::string& value, int fallback = 1) {
    try {
        const int parsed = std::stoi(trim(value));
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

CommandResult run_command(std::vector<std::string> args, bool asRoot = false, const std::string& input = "") {
    if (asRoot && geteuid() != 0) {
        args.insert(args.begin(), "sudo");
    }

    int outPipe[2] = {-1, -1};
    int inPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0) return {1, "pipe failed"};
    if (!input.empty() && pipe(inPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return {1, "pipe failed"};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        if (inPipe[0] != -1) {
            close(inPipe[0]);
            close(inPipe[1]);
        }
        return {1, std::strerror(errno)};
    }

    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        if (!input.empty()) {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[0]);
            close(inPipe[1]);
        }

        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(outPipe[1]);
    if (!input.empty()) {
        close(inPipe[0]);
        const char* data = input.data();
        size_t remaining = input.size();
        while (remaining > 0) {
            const ssize_t written = write(inPipe[1], data, remaining);
            if (written <= 0) break;
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        close(inPipe[1]);
    }

    std::string output;
    char buffer[512];
    ssize_t count = 0;
    while ((count = read(outPipe[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(count));
    }
    close(outPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return {1, output};
    if (WIFEXITED(status)) return {WEXITSTATUS(status), output};
    if (WIFSIGNALED(status)) return {128 + WTERMSIG(status), output};
    return {1, output};
}

int run_command_interactive(std::vector<std::string> args, bool asRoot = false) {
    if (asRoot && geteuid() != 0) {
        args.insert(args.begin(), "sudo");
    }

    const pid_t pid = fork();
    if (pid < 0) return 1;

    if (pid == 0) {
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

bool update_output_completed(const std::string& output) {
    const std::string value = lower(output);
    return value.find("update completed") != std::string::npos ||
           value.find("updated successfully") != std::string::npos;
}

bool update_output_up_to_date(const std::string& output) {
    return lower(output).find("already on newest version") != std::string::npos;
}

std::string read_file_text(const fs::path& path) {
    std::ifstream file(path);
    if (!file) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

CommandResult run_command_with_progress(
    std::vector<std::string> args,
    bool asRoot = false,
    const std::string& input = "",
    const std::string& title = "KSM UPDATE",
    const std::string& successStatus = "Update completed.",
    const std::string& failureStatus = "Update failed. Check /tmp/kupgr.log.",
    const std::vector<std::string>& customSteps = {}
) {
    if (asRoot && geteuid() != 0) {
        args.insert(args.begin(), "sudo");
    }

    ProgressState state;
    state.title = title;
    if (!customSteps.empty()) state.steps = customSteps;
    state.status = "Starting...";
    auto screen = ScreenInteractive::Fullscreen();
    auto exitLoop = screen.ExitLoopClosure();

    std::thread worker([&] {
        auto finish = [&](int code, const std::string& status) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.exitCode = code;
            state.done = true;
            state.ok = code == 0;
            state.currentStep = state.ok ? static_cast<int>(state.steps.size()) - 1 : state.currentStep;
            state.status = status;
            screen.PostEvent(Event::Custom);
        };

        int outPipe[2] = {-1, -1};
        int inPipe[2] = {-1, -1};
        if (pipe(outPipe) != 0) {
            finish(1, "pipe failed");
            return;
        }
        if (!input.empty() && pipe(inPipe) != 0) {
            close(outPipe[0]);
            close(outPipe[1]);
            finish(1, "pipe failed");
            return;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            close(outPipe[0]);
            close(outPipe[1]);
            if (inPipe[0] != -1) {
                close(inPipe[0]);
                close(inPipe[1]);
            }
            finish(1, std::strerror(errno));
            return;
        }

        if (pid == 0) {
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(outPipe[1], STDERR_FILENO);
            if (!input.empty()) dup2(inPipe[0], STDIN_FILENO);
            close(outPipe[0]);
            close(outPipe[1]);
            if (inPipe[0] != -1) {
                close(inPipe[0]);
                close(inPipe[1]);
            }

            std::vector<char*> argv;
            for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }

        close(outPipe[1]);
        if (!input.empty()) {
            close(inPipe[0]);
            const char* data = input.data();
            size_t remaining = input.size();
            while (remaining > 0) {
                const ssize_t written = write(inPipe[1], data, remaining);
                if (written <= 0) break;
                data += written;
                remaining -= static_cast<size_t>(written);
            }
            close(inPipe[1]);
        }

        std::string pendingLine;
        char buffer[512];
        ssize_t count = 0;
        while ((count = read(outPipe[0], buffer, sizeof(buffer))) > 0) {
            std::string chunk(buffer, static_cast<size_t>(count));
            for (char ch : chunk) {
                if (ch == '\r') continue;
                if (ch == '\n') {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    update_progress_from_line(state, pendingLine);
                    pendingLine.clear();
                    screen.PostEvent(Event::Custom);
                } else {
                    pendingLine += ch;
                }
            }
        }
        close(outPipe[0]);

        if (!pendingLine.empty()) {
            std::lock_guard<std::mutex> lock(state.mutex);
            update_progress_from_line(state, pendingLine);
            screen.PostEvent(Event::Custom);
        }

        int status = 0;
        int code = 1;
        if (waitpid(pid, &status, 0) == 0) {
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            const bool ksmUpdate = title == "KSM UPDATE";
            const std::string logText = ksmUpdate ? read_file_text("/tmp/kupgr.log") : "";
            const std::string combinedOutput = state.output + "\n" + logText;
            if (ksmUpdate && update_output_up_to_date(combinedOutput)) state.upToDate = true;
            if (ksmUpdate && update_output_completed(combinedOutput)) state.completed = true;

            const bool logicalSuccess = code == 0 || state.completed || state.upToDate;
            state.exitCode = logicalSuccess ? 0 : code;
            state.done = true;
            state.ok = logicalSuccess;
            if (state.ok) {
                state.currentStep = static_cast<int>(state.steps.size()) - 1;
                state.status = state.upToDate ? "Already on newest version." : successStatus;
            } else {
                state.status = failureStatus;
            }
        }
        screen.PostEvent(Event::Custom);
    });

    auto root = Renderer([&] {
        std::lock_guard<std::mutex> lock(state.mutex);
        return render_progress(state);
    });

    root = CatchEvent(root, [&](Event event) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.done) return true;
        if (event == Event::Return || event == Event::Escape ||
            event == Event::Character("q") || event == Event::Character("Q")) {
            exitLoop();
            return true;
        }
        return true;
    });

    screen.Loop(root);
    if (worker.joinable()) worker.join();

    std::lock_guard<std::mutex> lock(state.mutex);
    return {state.exitCode, state.output};
}

std::string shell_output(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    pclose(pipe);
    return trim(output);
}

bool command_exists(const std::string& command) {
    return run_command({"sh", "-c", "command -v " + command + " >/dev/null 2>&1"}).code == 0;
}

bool zpm_installed() {
    std::error_code ec;
    return fs::exists("/opt/ZPM", ec) && fs::is_directory("/opt/ZPM", ec);
}

std::vector<std::string> split_words(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string word;
    while (stream >> word) result.push_back(word);
    return result;
}

std::string zpm_executable(const std::string& name) {
    const std::vector<std::string> candidates = {
        "/opt/ZPM/bin/" + name,
        "/usr/bin/" + name,
        "/usr/local/bin/" + name
    };
    for (const auto& candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return name;
}

std::vector<ListItem> zpm_function_items() {
    return {
        {"zinst", "zinst", "Install packages", false, false, {}},
        {"zrm", "zrm", "Remove packages", false, false, {}},
        {"zsearch", "zsearch", "Search packages", false, false, {}},
        {"zinfo", "zinfo", "Package information", false, false, {}},
        {"zlist", "zlist", "List installed packages", false, false, {}},
        {"zrun", "zrun", "Run installed program", false, false, {}},
        {"zupd", "zupd", "Update system packages", false, false, {}},
        {"zupgr", "zupgr", "Upgrade ZPM itself", false, false, {}},
        {"zclean", "zclean", "Clean package caches", false, false, {}},
        {"zhome", "zhome", "Show ZPM guide pages", false, false, {}},
        {"ztui", "ztui", "Open ZPM TUI", false, false, {}},
        {"zuninstall", "zuninstall", "Uninstall ZPM", false, false, {}}
    };
}

void load_zpm_function_panel(NativePanel& panel, const std::string& command) {
    panel.selectedKey = command;
    panel.rows.clear();
    panel.detailLines.clear();
    panel.armedAction.clear();

    auto addRun = [&](const std::string& label, bool danger = false) {
        panel.rows.push_back({RowType::Action, "run_zpm_gui", label, "", {}, false, danger});
    };

    if (command == "zinst") {
        panel.rows = {
            {RowType::Text, "packages", "Package name(s)", ""},
            {RowType::Bool, "dry_run", "Dry run", "", {}, false, false}
        };
        addRun("Run zinst");
        panel.detailLines = {"Install packages from native PM, Flatpak or Snap."};
    } else if (command == "zrm") {
        panel.rows = {
            {RowType::Text, "packages", "Package name(s)", ""},
            {RowType::Bool, "purge", "Purge", "", {}, false, false},
            {RowType::Bool, "dry_run", "Dry run", "", {}, false, false}
        };
        addRun("Run zrm", true);
        panel.detailLines = {"Remove packages. Purge is APT-only."};
    } else if (command == "zsearch") {
        panel.rows = {
            {RowType::Text, "query", "Search query", ""},
            {RowType::Choice, "source", "Source", "all", {"all", "native", "flatpak", "snap"}}
        };
        addRun("Run zsearch");
        panel.detailLines = {"Search package sources."};
    } else if (command == "zinfo") {
        panel.rows = {{RowType::Text, "packages", "Package name(s)", ""}};
        addRun("Run zinfo");
        panel.detailLines = {"Show package information."};
    } else if (command == "zlist") {
        panel.rows = {
            {RowType::Choice, "source", "Source", "all", {"all", "native", "flatpak", "snap"}},
            {RowType::Bool, "no_pager", "No pager", "", {}, true, false}
        };
        addRun("Run zlist");
        panel.detailLines = {"List installed packages."};
    } else if (command == "zrun") {
        panel.rows = {{RowType::Text, "package", "Program/package", ""}};
        addRun("Run zrun");
        panel.detailLines = {"Find and launch an installed program."};
    } else if (command == "zupd") {
        panel.rows = {
            {RowType::Bool, "full", "Full upgrade", "", {}, false, false},
            {RowType::Bool, "yes", "Assume yes", "", {}, false, false},
            {RowType::Bool, "reboot", "Reboot after", "", {}, false, false},
            {RowType::Bool, "shutdown", "Shutdown after", "", {}, false, false},
            {RowType::Bool, "dry_run", "Dry run", "", {}, false, false}
        };
        addRun("Run zupd", true);
        panel.detailLines = {"Update system packages."};
    } else if (command == "zupgr") {
        panel.rows = {
            {RowType::Bool, "force", "Force", "", {}, false, false},
            {RowType::Bool, "experimental", "Experimental", "", {}, false, false},
            {RowType::Bool, "dry_run", "Dry run", "", {}, false, false}
        };
        addRun("Run zupgr", true);
        panel.detailLines = {"Upgrade ZPM itself."};
    } else if (command == "zclean") {
        panel.rows = {{RowType::Bool, "dry_run", "Dry run", "", {}, false, false}};
        addRun("Run zclean", true);
        panel.detailLines = {"Clean package manager caches and unused data."};
    } else if (command == "zhome") {
        panel.rows = {
            {RowType::Choice, "page", "Page", "default", {"default", "page 1", "page 2", "page 3"}},
            {RowType::Bool, "edit_config", "Edit config", "", {}, false, false}
        };
        addRun("Run zhome");
        panel.detailLines = {"Open ZPM help pages or edit ZPM config."};
    } else if (command == "ztui") {
        addRun("Run ztui");
        panel.detailLines = {"Open the original ZPM TUI."};
    } else if (command == "zuninstall") {
        addRun("Run zuninstall", true);
        panel.detailLines = {"Uninstall ZPM. The ZPM command will ask for confirmation."};
    } else {
        addRun("Run selected ZPM command");
        panel.detailLines = {"Unknown ZPM command."};
    }

    panel.rowIndex = 0;
    panel.focus = PanelFocus::Rows;
    panel.message = "Selected ZPM function: " + command + ".";
}

std::string strip_ansi(const std::string& value) {
    std::string clean;
    bool escape = false;
    for (char ch : value) {
        if (!escape && ch == '\033') {
            escape = true;
            continue;
        }
        if (escape) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                escape = false;
            }
            continue;
        }
        clean += ch;
    }
    return clean;
}

void push_progress_log(ProgressState& state, const std::string& line) {
    if (line.empty()) return;
    state.logLines.push_back(line);
    if (state.logLines.size() > 8) state.logLines.erase(state.logLines.begin());
}

void update_progress_from_line(ProgressState& state, const std::string& line) {
    const std::string cleanLine = strip_ansi(line);
    state.output += cleanLine + "\n";
    push_progress_log(state, cleanLine);
    state.status = cleanLine;

    const std::string lowerLine = lower(cleanLine);
    auto step = [&](int index, const std::string& status) {
        state.currentStep = std::max(state.currentStep, index);
        state.status = status;
    };

    if (lowerLine.find("checking required tools") != std::string::npos) step(1, "Checking required tools...");
    else if (lowerLine.find("detected package manager") != std::string::npos) step(1, "Checking system...");
    else if (lowerLine.find("zpm dependencies") != std::string::npos ||
             lowerLine.find("installing dependencies") != std::string::npos) step(2, "Installing dependencies...");
    else if (lowerLine.find("fetching latest version") != std::string::npos) step(3, "Fetching latest ZPM release...");
    else if (lowerLine.find("downloading") != std::string::npos && lowerLine.find("zpm") != std::string::npos) step(4, "Downloading ZPM...");
    else if (lowerLine.find("extracting archive") != std::string::npos) step(5, "Extracting ZPM...");
    else if (lowerLine.find("installing to /opt/zpm") != std::string::npos ||
             lowerLine.find("files copied to /opt/zpm") != std::string::npos) step(6, "Installing ZPM files...");
    else if (lowerLine.find("recompiling zpm") != std::string::npos ||
             lowerLine.find("recompilation complete") != std::string::npos) step(7, "Compiling ZPM...");
    else if (lowerLine.find("updating symlinks") != std::string::npos ||
             lowerLine.find("symlinks created") != std::string::npos) step(8, "Linking ZPM commands...");
    else if (lowerLine.find("installation complete") != std::string::npos) {
        state.completed = true;
        step(9, "ZPM installation complete.");
    }
    else if (lowerLine.find("fetching github release metadata") != std::string::npos) step(2, "Fetching release metadata...");
    else if (lowerLine.find("already on newest version") != std::string::npos) {
        state.upToDate = true;
        step(9, "Already on newest version.");
    } else if (lowerLine.find("downloading ksm") != std::string::npos) step(3, "Downloading archive...");
    else if (lowerLine.find("extracting source archive") != std::string::npos) step(4, "Extracting source...");
    else if (lowerLine.find("writing version.txt") != std::string::npos) step(5, "Writing VERSION.txt...");
    else if (lowerLine.find("building c++ programs") != std::string::npos) step(6, "Building C++ programs...");
    else if (lowerLine.find("preparing new installation tree") != std::string::npos) step(7, "Preparing installation tree...");
    else if (lowerLine.find("activating new ksm installation") != std::string::npos) step(8, "Activating installation...");
    else if (lowerLine.find("update completed") != std::string::npos ||
             lowerLine.find("updated successfully") != std::string::npos) {
        state.completed = true;
        step(9, "Update completed.");
    } else if (lowerLine.find("failed") != std::string::npos ||
               lowerLine.find("[x]") != std::string::npos) {
        state.status = "Update failed.";
    }
}

Element render_progress(const ProgressState& state) {
    const int stepCount = static_cast<int>(state.steps.size());
    const float progress = stepCount <= 1
        ? 1.0f
        : static_cast<float>(state.currentStep) / static_cast<float>(stepCount - 1);

    Elements stepRows;
    for (int i = 0; i < stepCount; ++i) {
        std::string mark = " ";
        Color stepColor = Color::Blue;
        if (i < state.currentStep || state.done) {
            mark = "x";
            stepColor = Color::Green;
        } else if (i == state.currentStep) {
            mark = ">";
            stepColor = Color::Cyan;
        }

        auto label = text(state.steps[i]);
        if (i <= state.currentStep || state.done) label = label | color(stepColor);
        else label = label | dim;

        stepRows.push_back(hbox({
            text("[" + mark + "] ") | color(stepColor) | bold,
            label
        }));
    }

    Elements logs;
    for (const auto& line : state.logLines) {
        logs.push_back(text(fit_text(line, 86)) | dim);
    }
    if (logs.empty()) logs.push_back(text("Waiting for updater output...") | dim);

    auto titleColor = state.done ? (state.ok ? Color::Green : Color::Red) : Color::Cyan;
    Element finalText;
    if (state.done) {
        finalText = text(state.ok ? (state.upToDate ? "ALREADY UP TO DATE" : "UPDATE COMPLETED") : "UPDATE FAILED") |
            bold | color(titleColor) | hcenter;
    } else {
        finalText = text(state.title) | bold | color(Color::Cyan) | hcenter;
    }

    return vbox({
        finalText,
        separator(),
        hbox({
            vbox({
                text(" Steps ") | bold | color(Color::Cyan),
                vbox(std::move(stepRows)) | flex
            }) | borderStyled(Color::Blue) | size(WIDTH, EQUAL, 28),
            text("  "),
            vbox({
                text(" Progress ") | bold | color(Color::Cyan),
                gauge(progress) | color(state.done && !state.ok ? Color::Red : Color::Green),
                text(std::to_string(std::min(100, static_cast<int>(progress * 100.0f))) + "%") | hcenter,
                separator(),
                paragraph(state.status) | color(state.done ? titleColor : Color::Yellow),
                separator(),
                text(" Logs ") | bold | color(Color::Cyan),
                vbox(std::move(logs)) | flex
            }) | borderStyled(Color::Blue) | flex
        }) | flex,
        separator(),
        text(state.done ? "Press Enter to return." : "Update is running. Please wait.") | dim | hcenter
    }) | borderStyled(titleColor);
}

bool ensure_root_auth(std::string& message) {
    if (geteuid() == 0) return true;
    if (!command_exists("sudo")) {
        message = "This action needs root, and sudo is not available.";
        return false;
    }
    const CommandResult result = run_command({"sudo", "-v"});
    if (result.code != 0) {
        message = "Root authorization failed.";
        return false;
    }
    return true;
}

std::vector<ListItem> read_users(bool showSystemUsers, bool selectable) {
    std::vector<ListItem> users;
    setpwent();
    while (struct passwd* pw = getpwent()) {
        const std::string name = pw->pw_name ? pw->pw_name : "";
        if (name.empty() || name == "root") continue;
        if (!showSystemUsers && (pw->pw_uid < 1000 || name == "nobody")) continue;

        ListItem item;
        item.key = name;
        item.label = name;
        std::ostringstream detail;
        detail << "uid:" << pw->pw_uid << " home:" << (pw->pw_dir ? pw->pw_dir : "-");
        item.detail = detail.str();
        item.selected = selectable ? false : item.selected;
        users.push_back(item);
    }
    endpwent();

    std::sort(users.begin(), users.end(), [](const ListItem& a, const ListItem& b) {
        return a.label < b.label;
    });
    return users;
}

std::vector<ListItem> read_groups(bool showSystemGroups, bool selectable) {
    std::vector<ListItem> groups;
    std::ifstream file("/etc/group");
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, ':')) parts.push_back(part);
        if (parts.size() < 3 || parts[0].empty()) continue;

        int gid = 0;
        try {
            gid = std::stoi(parts[2]);
        } catch (...) {
            gid = 0;
        }
        if (!showSystemGroups && gid < 1000) continue;

        ListItem item;
        item.key = parts[0];
        item.label = parts[0];
        item.detail = "gid:" + parts[2];
        if (parts.size() >= 4 && !parts[3].empty()) item.detail += " members:" + parts[3];
        item.selected = selectable ? false : item.selected;
        groups.push_back(item);
    }
    std::sort(groups.begin(), groups.end(), [](const ListItem& a, const ListItem& b) {
        return a.label < b.label;
    });
    return groups;
}

std::vector<ListItem> read_services() {
    std::vector<ListItem> services;
    FILE* pipe = popen("systemctl list-units --type=service --all --no-legend --no-pager 2>/dev/null", "r");
    if (!pipe) return services;

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        std::istringstream ss(line);
        std::string name;
        std::string load;
        std::string active;
        std::string sub;
        ss >> name >> load >> active >> sub;
        std::string description;
        std::getline(ss, description);
        description = trim(description);
        if (name.empty()) continue;

        ListItem item;
        item.key = name;
        item.label = name;
        item.detail = active + "/" + sub;
        if (!description.empty()) item.detail += "  " + description;
        services.push_back(item);
    }
    pclose(pipe);
    return services;
}

std::vector<ListItem> read_interfaces() {
    std::vector<ListItem> interfaces;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/net", ec)) {
        if (ec) break;
        ListItem item;
        item.key = entry.path().filename().string();
        item.label = item.key;
        item.detail = trim(shell_output("cat /sys/class/net/" + item.key + "/operstate 2>/dev/null"));
        if (item.detail.empty()) item.detail = "unknown";
        interfaces.push_back(item);
    }
    std::sort(interfaces.begin(), interfaces.end(), [](const ListItem& a, const ListItem& b) {
        return a.label < b.label;
    });
    return interfaces;
}

std::vector<ListItem> read_directory(const fs::path& directory) {
    std::vector<ListItem> entries;

    ListItem current;
    current.key = "__select__";
    current.label = "[select this directory]";
    current.detail = directory.lexically_normal().string();
    current.directory = false;
    current.path = directory;
    entries.push_back(current);

    ListItem up;
    up.key = "__up__";
    up.label = "[..]";
    up.detail = "go up";
    up.directory = true;
    up.path = directory.parent_path();
    entries.push_back(up);

    std::vector<ListItem> rest;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        ListItem item;
        item.path = entry.path();
        item.directory = entry.is_directory(ec);
        item.key = item.path.string();
        item.label = item.path.filename().string() + (item.directory ? "/" : "");
        item.detail = item.directory ? "directory" : "file";
        rest.push_back(item);
    }

    std::sort(rest.begin(), rest.end(), [](const ListItem& a, const ListItem& b) {
        if (a.directory != b.directory) return a.directory > b.directory;
        return a.label < b.label;
    });
    entries.insert(entries.end(), rest.begin(), rest.end());
    return entries;
}

std::vector<ListItem> picker_groups(const std::string& current) {
    std::vector<ListItem> groups = read_groups(true, true);
    const auto selected = split_csv(current);
    for (auto& group : groups) {
        group.selected = contains_value(selected, group.key);
    }
    return groups;
}

std::vector<ListItem> picker_users(const std::string& current) {
    std::vector<ListItem> users = read_users(true, true);
    const auto selected = split_csv(current);
    for (auto& user : users) {
        user.selected = contains_value(selected, user.key);
    }
    return users;
}

void refresh_sysinfo(NativePanel& panel) {
    panel.detailLines.clear();
    panel.list.clear();

    auto add = [&](const std::string& label, const std::string& command) {
        ListItem item;
        item.key = label;
        item.label = label;
        item.detail = shell_output(command);
        if (item.detail.empty()) item.detail = "-";
        panel.list.push_back(item);
    };

    add("Hostname", "hostname 2>/dev/null");
    add("Kernel", "uname -srmo 2>/dev/null");
    add("Uptime", "uptime -p 2>/dev/null");
    add("Load", "cat /proc/loadavg 2>/dev/null");
    add("Disk /", "df -h / 2>/dev/null | awk 'NR==2 {print $3 \" used of \" $2 \" (\" $5 \")\"}'");
    add("Memory", "free -h 2>/dev/null | awk '/Mem:/ {print $3 \" used of \" $2}'");
    add("Failed units", "systemctl --failed --no-legend 2>/dev/null | wc -l");

    panel.listIndex = 0;
    panel.message = "System snapshot refreshed.";
}

void load_user_into_panel(NativePanel& panel, const std::string& username) {
    struct passwd* pw = getpwnam(username.c_str());
    if (!pw) {
        panel.message = "User not found.";
        return;
    }
    panel.selectedKey = username;
    set_row_value(panel, "new_login", "");
    set_row_value(panel, "full_name", pw->pw_gecos ? pw->pw_gecos : "");
    set_row_value(panel, "home", pw->pw_dir ? pw->pw_dir : "");
    set_row_value(panel, "shell", pw->pw_shell ? pw->pw_shell : "");
    set_row_value(panel, "groups", "");
    panel.message = "Loaded user " + username + ".";
}

void load_group_into_panel(NativePanel& panel, const std::string& groupName) {
    std::ifstream file("/etc/group");
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, ':')) parts.push_back(part);
        if (parts.size() >= 4 && parts[0] == groupName) {
            panel.selectedKey = groupName;
            set_row_value(panel, "new_name", "");
            set_row_value(panel, "gid", parts[2]);
            set_row_value(panel, "members", parts[3]);
            panel.message = "Loaded group " + groupName + ".";
            return;
        }
    }
    panel.message = "Group not found.";
}

void load_perm_current(NativePanel& panel) {
    const std::string path = row_value(panel, "path");
    struct stat st {};
    if (path.empty() || stat(path.c_str(), &st) != 0) {
        panel.message = "Path not found.";
        return;
    }

    std::ostringstream mode;
    mode << std::oct << (st.st_mode & 0777);
    set_row_value(panel, "mode", mode.str());
    if (struct passwd* pw = getpwuid(st.st_uid)) set_row_value(panel, "owner", pw->pw_name);
    else set_row_value(panel, "owner", std::to_string(st.st_uid));
    if (struct group* gr = getgrgid(st.st_gid)) set_row_value(panel, "group", gr->gr_name);
    else set_row_value(panel, "group", std::to_string(st.st_gid));
    panel.message = "Loaded current permissions.";
}

void load_network_current(NativePanel& panel) {
    const std::string iface = panel.selectedKey;
    if (iface.empty()) {
        panel.message = "Select an interface first.";
        return;
    }
    const std::string addr = shell_output("ip -o -4 addr show dev " + iface + " scope global 2>/dev/null | awk '{print $4}' | head -n1");
    const std::string gateway = shell_output("ip route show default dev " + iface + " 2>/dev/null | awk '{print $3}' | head -n1");
    std::string dns;
    if (command_exists("resolvectl")) {
        dns = shell_output("resolvectl dns " + iface + " 2>/dev/null | sed 's/.*: //'");
    }
    if (dns.empty()) {
        dns = shell_output("awk '/^nameserver/ {printf \"%s%s\", sep, $2; sep=\",\"}' /etc/resolv.conf 2>/dev/null");
    }
    set_row_value(panel, "address", addr);
    set_row_value(panel, "gateway", gateway);
    set_row_value(panel, "dns", dns);
    panel.message = "Loaded current interface settings.";
}

void load_ssh_config(NativePanel& panel) {
    std::ifstream file("/etc/ssh/sshd_config");
    if (!file) {
        panel.message = "Could not read sshd_config, using defaults.";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string clean = trim(line);
        if (clean.empty()) continue;
        if (clean[0] == '#') clean = trim(clean.substr(1));
        std::istringstream ss(clean);
        std::string key;
        std::string value;
        ss >> key >> value;
        key = lower(key);
        if (key == "port" && !value.empty()) set_row_value(panel, "port", value);
        if (key == "permitrootlogin" && !value.empty()) set_row_value(panel, "root", value);
        if (key == "passwordauthentication" && !value.empty()) set_row_value(panel, "password", value);
        if (key == "pubkeyauthentication" && !value.empty()) set_row_value(panel, "pubkey", value);
        if (key == "x11forwarding" && !value.empty()) set_row_value(panel, "x11", value);
    }
    panel.message = "Loaded sshd_config.";
}

std::string detect_package_manager() {
    if (command_exists("apt-get")) return "apt";
    if (command_exists("zypper")) return "zypper";
    if (command_exists("dnf")) return "dnf";
    return "unknown";
}

std::vector<std::string> extension_packages(const std::string& feature, const std::string& pm) {
    if (feature == "samba") return {"samba"};
    if (feature == "apache") return {pm == "dnf" ? "httpd" : "apache2"};
    if (feature == "dns") return {pm == "apt" ? "bind9" : "bind"};
    if (feature == "dhcp") return {pm == "apt" ? "isc-dhcp-server" : "dhcp-server"};
    if (feature == "ftp") return {"vsftpd"};
    return {};
}

CommandResult install_extension_packages(const std::string& feature) {
    const std::string pm = detect_package_manager();
    const std::vector<std::string> packages = extension_packages(feature, pm);
    if (pm == "unknown" || packages.empty()) return {1, "No supported package manager found."};

    std::vector<std::string> args;
    if (pm == "apt") args = {"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y"};
    else if (pm == "zypper") args = {"zypper", "--non-interactive", "install", "-y"};
    else args = {"dnf", "install", "-y"};

    args.insert(args.end(), packages.begin(), packages.end());
    return run_command(args, true);
}

std::string service_status(const std::vector<std::string>& services) {
    for (const auto& service : services) {
        const CommandResult result = run_command({"systemctl", "is-active", service});
        const std::string state = trim(result.output);
        if (!state.empty() && state != "unknown") return service + ": " + state;
    }
    return "not installed";
}

bool service_action(const std::vector<std::string>& services, const std::vector<std::string>& action) {
    for (const auto& service : services) {
        std::vector<std::string> args = {"systemctl"};
        args.insert(args.end(), action.begin(), action.end());
        args.push_back(service);
        if (run_command(args, true).code == 0) return true;
    }
    return false;
}

bool write_root_file(const fs::path& path, const std::string& content, bool backup = true) {
    const fs::path temp = fs::path("/tmp") / ("ksm-write-" + std::to_string(getpid()) + "-" + path.filename().string());
    {
        std::ofstream file(temp);
        if (!file) return false;
        file << content;
    }

    int failures = 0;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        if (run_command({"mkdir", "-p", parent.string()}, true).code != 0) ++failures;
    }
    std::error_code ec;
    if (backup && fs::exists(path, ec)) {
        run_command({"cp", path.string(), path.string() + ".ksm.bak"}, true);
    }
    if (run_command({"cp", temp.string(), path.string()}, true).code != 0) ++failures;
    fs::remove(temp, ec);
    return failures == 0;
}

void refresh_server_status(NativePanel& panel, const std::string& rowId, const std::vector<std::string>& services) {
    set_row_value(panel, rowId, service_status(services));
}

NativePanel make_native_panel(const Module& module) {
    NativePanel panel;
    panel.kind = module.kind;
    panel.title = module.title;
    panel.subtitle = module.description;

    switch (module.kind) {
    case ModuleKind::Home:
        panel.rows = {
            {RowType::Choice, "page", "Page", "KSM overview", {"KSM overview", "Commands", "Panel controls"}},
            {RowType::Action, "refresh_home", "Refresh panel text", "", {}, false, false}
        };
        panel.detailLines = {
            "Kastiusz System Manager is controlled from this FTXUI panel.",
            "Public entrypoint: sudo ksm.",
            "Internal helpers stay in /opt/KSM/bin and are not public commands."
        };
        break;
    case ModuleKind::SysInfo:
        panel.rows = {{RowType::Action, "refresh_sysinfo", "Refresh snapshot", "", {}, false, false}};
        refresh_sysinfo(panel);
        break;
    case ModuleKind::Upgrade:
        panel.rows = {
            {RowType::Bool, "experimental", "Experimental channel", "", {}, contains_value(module.args, "-ex"), false, "Prerelease channel"},
            {RowType::Bool, "repo", "Repo snapshot", "", {}, false, false, "Only with experimental"},
            {RowType::Bool, "force", "Force reinstall", "", {}, false, false},
            {RowType::Action, "run_upgrade", "Start upgrade", "", {}, false, true}
        };
        panel.message = "Upgrade runs from this panel with selected flags.";
        break;
    case ModuleKind::UserAdd:
        panel.rows = {
            {RowType::Text, "amount", "Amount", "1"},
            {RowType::Text, "username", "Username template", ""},
            {RowType::Text, "password", "Password template", ""},
            {RowType::Text, "full_name", "Full name template", ""},
            {RowType::Bool, "create_home", "Create home", "", {}, true, false},
            {RowType::Text, "shell", "Login shell", "/bin/bash"},
            {RowType::Text, "home", "Home path template", ""},
            {RowType::Text, "groups", "Extra groups", "", {}, false, false, "Enter opens group picker"},
            {RowType::Bool, "sudo", "Add sudo group", "", {}, false, false},
            {RowType::Action, "create_users", "Create users", "", {}, false, true}
        };
        panel.detailLines = {
            "Use $i in templates to insert the generated number.",
            "Example: user$i creates user1, user2, user3.",
            "If Amount is greater than 1 and Username template has no $i, KSM appends $i automatically."
        };
        break;
    case ModuleKind::UserDel:
        panel.multiList = true;
        panel.focus = PanelFocus::List;
        panel.list = read_users(false, true);
        panel.rows = {
            {RowType::Bool, "remove_home", "Remove home directory", "", {}, true, false},
            {RowType::Bool, "force", "Force delete", "", {}, false, false},
            {RowType::Bool, "show_system", "Show system users", "", {}, false, false},
            {RowType::Text, "name", "Select by name", ""},
            {RowType::Action, "select_name", "Select name matches", "", {}, false, false},
            {RowType::Action, "refresh_users", "Reload users", "", {}, false, false},
            {RowType::Action, "delete_users", "Delete selected", "", {}, false, true}
        };
        break;
    case ModuleKind::UserMod:
        panel.focus = PanelFocus::List;
        panel.list = read_users(false, false);
        panel.rows = {
            {RowType::Bool, "show_system", "Show system users", "", {}, false, false},
            {RowType::Text, "new_login", "New login", ""},
            {RowType::Text, "full_name", "Full name", ""},
            {RowType::Text, "home", "Home path", ""},
            {RowType::Bool, "move_home", "Move home", "", {}, false, false},
            {RowType::Text, "shell", "Login shell", ""},
            {RowType::Text, "groups", "Groups", "", {}, false, false, "Enter opens group picker"},
            {RowType::Bool, "append_groups", "Append groups", "", {}, true, false},
            {RowType::Bool, "lock", "Lock password", "", {}, false, false},
            {RowType::Bool, "unlock", "Unlock password", "", {}, false, false},
            {RowType::Action, "apply_usermod", "Apply user changes", "", {}, false, true}
        };
        break;
    case ModuleKind::GroupAdd:
        panel.rows = {
            {RowType::Text, "amount", "Amount", "1"},
            {RowType::Text, "name", "Group template", ""},
            {RowType::Text, "gid", "GID start", ""},
            {RowType::Bool, "system", "System group", "", {}, false, false},
            {RowType::Text, "members", "Members", "", {}, false, false, "Enter opens user picker"},
            {RowType::Action, "create_groups", "Create groups", "", {}, false, true}
        };
        panel.detailLines = {
            "Use $i in the group template to insert the generated number.",
            "Example: team$i creates team1, team2, team3.",
            "If Amount is greater than 1 and Group template has no $i, KSM appends $i automatically."
        };
        break;
    case ModuleKind::GroupDel:
        panel.multiList = true;
        panel.focus = PanelFocus::List;
        panel.list = read_groups(false, true);
        panel.rows = {
            {RowType::Bool, "force", "Force delete", "", {}, false, false},
            {RowType::Bool, "show_system", "Show system groups", "", {}, false, false},
            {RowType::Text, "name", "Select by name", ""},
            {RowType::Action, "select_name", "Select name matches", "", {}, false, false},
            {RowType::Text, "keyword", "Delete with keyword", ""},
            {RowType::Action, "select_keyword", "Select keyword matches", "", {}, false, false},
            {RowType::Action, "refresh_groups", "Reload groups", "", {}, false, false},
            {RowType::Action, "delete_groups", "Delete selected", "", {}, false, true}
        };
        break;
    case ModuleKind::GroupMod:
        panel.focus = PanelFocus::List;
        panel.list = read_groups(false, false);
        panel.rows = {
            {RowType::Bool, "show_system", "Show system groups", "", {}, false, false},
            {RowType::Text, "new_name", "New name", ""},
            {RowType::Text, "gid", "GID", ""},
            {RowType::Text, "members", "Members", "", {}, false, false, "Enter opens user picker"},
            {RowType::Action, "apply_groupmod", "Apply group changes", "", {}, false, true}
        };
        break;
    case ModuleKind::Services:
        panel.focus = PanelFocus::List;
        panel.list = read_services();
        panel.rows = {
            {RowType::Action, "start_service", "Start", "", {}, false, false},
            {RowType::Action, "stop_service", "Stop", "", {}, false, true},
            {RowType::Action, "restart_service", "Restart", "", {}, false, true},
            {RowType::Action, "enable_service", "Enable", "", {}, false, false},
            {RowType::Action, "disable_service", "Disable", "", {}, false, true},
            {RowType::Action, "reload_services", "Reload list", "", {}, false, false}
        };
        break;
    case ModuleKind::Permissions: {
        std::error_code ec;
        panel.directory = fs::current_path(ec);
        panel.focus = PanelFocus::List;
        panel.list = read_directory(panel.directory);
        panel.rows = {
            {RowType::Text, "path", "Path", panel.directory.string()},
            {RowType::Text, "owner", "Owner", ""},
            {RowType::Text, "group", "Group", ""},
            {RowType::Text, "mode", "Mode", ""},
            {RowType::Bool, "recursive", "Recursive", "", {}, false, false},
            {RowType::Action, "load_perm", "Load current", "", {}, false, false},
            {RowType::Action, "apply_perm", "Apply chmod/chown", "", {}, false, true}
        };
        load_perm_current(panel);
        break;
    }
    case ModuleKind::Network:
        panel.focus = PanelFocus::List;
        panel.list = read_interfaces();
        panel.rows = {
            {RowType::Bool, "enable", "Enable interface", "", {}, true, false},
            {RowType::Bool, "dhcp", "DHCP", "", {}, true, false},
            {RowType::Bool, "flush", "Flush addresses", "", {}, true, false},
            {RowType::Text, "address", "Address CIDR", ""},
            {RowType::Text, "gateway", "Gateway", ""},
            {RowType::Text, "dns", "DNS servers", ""},
            {RowType::Action, "load_network", "Load current", "", {}, false, false},
            {RowType::Action, "apply_network", "Apply runtime config", "", {}, false, true}
        };
        break;
    case ModuleKind::Power:
        panel.rows = {
            {RowType::Text, "delay", "Delay minutes", "0"},
            {RowType::Action, "reboot_now", "Reboot now", "", {}, false, true},
            {RowType::Action, "shutdown_now", "Shutdown now", "", {}, false, true},
            {RowType::Action, "shutdown_later", "Schedule shutdown", "", {}, false, true},
            {RowType::Action, "cancel_shutdown", "Cancel shutdown", "", {}, false, false}
        };
        panel.detailLines = {
            "Delay 0 means now.",
            "Reboot and shutdown use systemctl/shutdown directly."
        };
        break;
    case ModuleKind::SSH:
        panel.rows = {
            {RowType::Text, "port", "Port", "22"},
            {RowType::Choice, "root", "PermitRootLogin", "prohibit-password", {"yes", "no", "prohibit-password"}},
            {RowType::Choice, "password", "PasswordAuthentication", "no", {"yes", "no"}},
            {RowType::Choice, "pubkey", "PubkeyAuthentication", "yes", {"yes", "no"}},
            {RowType::Choice, "x11", "X11Forwarding", "no", {"yes", "no"}},
            {RowType::Bool, "restart", "Restart ssh service", "", {}, true, false},
            {RowType::Action, "load_ssh", "Load sshd_config", "", {}, false, false},
            {RowType::Action, "apply_ssh", "Apply sshd_config", "", {}, false, true}
        };
        load_ssh_config(panel);
        break;
    case ModuleKind::Firewall:
        panel.rows = {
            {RowType::Choice, "backend", "Backend", "auto", {"auto", "ufw", "firewalld"}},
            {RowType::Choice, "action", "Rule action", "allow", {"allow", "deny", "delete"}},
            {RowType::Text, "port", "Port", "22"},
            {RowType::Choice, "protocol", "Protocol", "tcp", {"tcp", "udp"}},
            {RowType::Action, "enable_firewall", "Enable firewall", "", {}, false, false},
            {RowType::Action, "apply_firewall", "Apply rule", "", {}, false, true}
        };
        break;
    case ModuleKind::Samba:
        panel.rows = {
            {RowType::Info, "status", "Service", service_status({"smbd", "samba"})},
            {RowType::Text, "share", "Share name", "public"},
            {RowType::Text, "path", "Share path", "/srv/samba/public"},
            {RowType::Bool, "create_path", "Create path", "", {}, true, false},
            {RowType::Bool, "writable", "Writable", "", {}, true, false},
            {RowType::Bool, "guest", "Guest access", "", {}, true, false},
            {RowType::Bool, "browseable", "Browseable", "", {}, true, false},
            {RowType::Action, "install_samba", "Install Samba extension", "", {}, false, false},
            {RowType::Action, "apply_samba", "Apply share", "", {}, false, true},
            {RowType::Action, "restart_samba", "Restart Samba", "", {}, false, true},
            {RowType::Action, "enable_samba", "Enable Samba", "", {}, false, false}
        };
        panel.detailLines = {
            "Creates or updates a KSM managed share block in smb.conf.",
            "Package is installed only when you press Install Samba extension."
        };
        break;
    case ModuleKind::Apache:
        panel.rows = {
            {RowType::Info, "status", "Service", service_status({"apache2", "httpd"})},
            {RowType::Text, "server_name", "ServerName", "_default_"},
            {RowType::Text, "port", "Port", "80"},
            {RowType::Text, "document_root", "DocumentRoot", "/var/www/ksm"},
            {RowType::Bool, "create_index", "Create index.html", "", {}, true, false},
            {RowType::Text, "index_title", "Index title", "KSM Apache"},
            {RowType::Action, "install_apache", "Install Apache extension", "", {}, false, false},
            {RowType::Action, "apply_apache", "Apply virtual host", "", {}, false, true},
            {RowType::Action, "restart_apache", "Restart Apache", "", {}, false, true},
            {RowType::Action, "enable_apache", "Enable Apache", "", {}, false, false}
        };
        panel.detailLines = {
            "Writes a simple KSM virtual host.",
            "Uses apache2 paths on Debian/openSUSE and httpd conf.d on Fedora/RHEL."
        };
        break;
    case ModuleKind::DnsServer:
        panel.rows = {
            {RowType::Info, "status", "Service", service_status({"bind9", "named"})},
            {RowType::Text, "forwarders", "Forwarders", "1.1.1.1,8.8.8.8"},
            {RowType::Bool, "listen_all", "Listen on all IPv4", "", {}, false, false},
            {RowType::Action, "install_dns", "Install DNS extension", "", {}, false, false},
            {RowType::Action, "apply_dns", "Apply DNS cache", "", {}, false, true},
            {RowType::Action, "restart_dns", "Restart DNS", "", {}, false, true},
            {RowType::Action, "enable_dns", "Enable DNS", "", {}, false, false}
        };
        panel.detailLines = {
            "Configures BIND as a simple caching DNS server with forwarders.",
            "Advanced zones can still be edited manually after this baseline."
        };
        break;
    case ModuleKind::DhcpServer: {
        panel.focus = PanelFocus::List;
        panel.list = read_interfaces();
        const std::string iface = panel.list.empty() ? "" : panel.list.front().key;
        panel.rows = {
            {RowType::Info, "status", "Service", service_status({"isc-dhcp-server", "dhcpd"})},
            {RowType::Text, "interface", "Interface", iface},
            {RowType::Text, "subnet", "Subnet", "192.168.1.0"},
            {RowType::Text, "netmask", "Netmask", "255.255.255.0"},
            {RowType::Text, "range_start", "Range start", "192.168.1.100"},
            {RowType::Text, "range_end", "Range end", "192.168.1.200"},
            {RowType::Text, "router", "Router", "192.168.1.1"},
            {RowType::Text, "dns", "DNS servers", "1.1.1.1,8.8.8.8"},
            {RowType::Action, "install_dhcp", "Install DHCP extension", "", {}, false, false},
            {RowType::Action, "apply_dhcp", "Apply DHCP config", "", {}, false, true},
            {RowType::Action, "restart_dhcp", "Restart DHCP", "", {}, false, true},
            {RowType::Action, "enable_dhcp", "Enable DHCP", "", {}, false, false}
        };
        panel.detailLines = {
            "Enter on an interface copies it into the Interface field.",
            "Writes a basic DHCP range config for ISC DHCP."
        };
        break;
    }
    case ModuleKind::FtpServer:
        panel.rows = {
            {RowType::Info, "status", "Service", service_status({"vsftpd"})},
            {RowType::Bool, "anonymous", "Anonymous login", "", {}, false, false},
            {RowType::Bool, "local_users", "Local users", "", {}, true, false},
            {RowType::Bool, "writable", "Write enable", "", {}, false, false},
            {RowType::Text, "banner", "Banner", "KSM FTP ready"},
            {RowType::Action, "install_ftp", "Install FTP extension", "", {}, false, false},
            {RowType::Action, "apply_ftp", "Apply vsftpd config", "", {}, false, true},
            {RowType::Action, "restart_ftp", "Restart FTP", "", {}, false, true},
            {RowType::Action, "enable_ftp", "Enable FTP", "", {}, false, false}
        };
        panel.detailLines = {
            "Configures the common vsftpd options.",
            "Anonymous and write access stay off unless explicitly enabled."
        };
        break;
    case ModuleKind::ExtensionInstall:
        panel.list = {
            {"zpm", "ZPM", zpm_installed() ? "installed in /opt/ZPM" : "available from GitHub", false, false, {}}
        };
        panel.rows = {
            {RowType::Info, "status", "ZPM status", zpm_installed() ? "installed" : "not installed"},
            {RowType::Action, "install_zpm", "Install ZPM extension", "", {}, false, true}
        };
        panel.detailLines = {
            "Source: github.com/Zielina-Konrad-productions/ZPM",
            "Extensions is the place for optional tools to install.",
            "After /opt/ZPM exists, KSM adds a separate ZPM tab.",
            "Install runs the ZPM INETINSTALL.sh script through bash."
        };
        panel.message = zpm_installed()
            ? "ZPM extension detected. Reopen menu to use the ZPM tab."
            : "ZPM is not installed yet. Use Install ZPM extension.";
        break;
    case ModuleKind::ZpmExtension:
        panel.focus = PanelFocus::List;
        panel.list = zpm_function_items();
        load_zpm_function_panel(panel, "zinst");
        panel.focus = PanelFocus::List;
        break;
    case ModuleKind::Uninstall:
        panel.rows = {
            {RowType::Bool, "links", "Remove command links", "", {}, true, false},
            {RowType::Bool, "opt", "Remove /opt/KSM", "", {}, true, false},
            {RowType::Bool, "force", "Force no prompt", "", {}, false, false},
            {RowType::Action, "run_uninstall", "Uninstall KSM", "", {}, false, true}
        };
        panel.message = "Danger zone. This panel performs the uninstall actions directly.";
        break;
    }

    return panel;
}

Element checkbox_element(bool checked) {
    if (checked) return text("[x]") | color(Color::Green) | bold;
    return text("[ ]") | dim;
}

Element value_element(const Row& row) {
    if (row.type == RowType::Bool) return checkbox_element(row.checked);
    if (row.value.empty()) return text(row.type == RowType::Action ? "Enter" : "(empty)") | dim;
    if (row.id == "password") return text(std::string(row.value.size(), '*')) | color(Color::Cyan);
    return text(fit_text(row.value, 42)) | color(Color::Cyan);
}

Element row_line(const Row& row, bool active, bool focused) {
    Element prefix = text(active ? ">" : " ") | size(WIDTH, EQUAL, 2);
    Element label = text(fit_text(row.label, 24)) | size(WIDTH, EQUAL, 26);
    Element body = value_element(row) | flex;
    auto line = hbox({prefix, label, body});

    if (row.danger && !active) line = line | color(Color::Red);
    if (row.type == RowType::Action && !row.danger && !active) line = line | color(Color::Green);
    if (active && focused) return line | bold | color(Color::White) | bgcolor(row.danger ? Color::Red : Color::Blue);
    if (active) return line | bold | color(row.danger ? Color::Red : Color::Cyan);
    return line;
}

Element list_line(const ListItem& item, bool active, bool focused, bool multi) {
    if (item.key.rfind("terminal", 0) == 0) {
        auto line = hbox({
            text(fit_text(item.label, 18)) | size(WIDTH, EQUAL, 20),
            text(item.detail) | flex
        });
        if (active && focused) return line | bold | color(Color::White) | bgcolor(Color::Blue);
        if (active) return line | bold | color(Color::Cyan);
        return line | color(Color::Green);
    }

    auto marker = multi ? checkbox_element(item.selected) : text(active ? ">" : " ");
    auto line = hbox({
        marker | size(WIDTH, EQUAL, 4),
        text(fit_text(item.label, 28)) | size(WIDTH, EQUAL, 30),
        text(fit_text(item.detail, 48)) | flex
    });

    if (active && focused) return line | bold | color(Color::White) | bgcolor(Color::Blue);
    if (active) return line | bold | color(Color::Cyan);
    if (item.directory) return line | color(Color::Cyan);
    return line;
}

Element category_line(const Category& category, bool active, bool focused) {
    auto line = hbox({
        text(active ? ">" : " ") | size(WIDTH, EQUAL, 2),
        text(category.title) | flex
    });
    if (active && focused) return line | bold | color(Color::White) | bgcolor(Color::Blue);
    if (active) return line | bold | color(Color::Cyan);
    return line | color(Color::Cyan);
}

Element module_line(const Module& module, bool active, bool focused) {
    auto line = hbox({
        text(active ? ">" : " ") | size(WIDTH, EQUAL, 2),
        text(fit_text(module.title, 22)) | size(WIDTH, EQUAL, 23),
        text(module.description) | flex
    });
    if (active && focused) return line | bold | color(Color::White) | bgcolor(Color::Blue);
    if (active) return line | bold | color(Color::Cyan);
    if (module.needsRoot) return line | color(Color::Yellow);
    return line | color(Color::Cyan);
}

Element action_button(const std::string& key, const std::string& label) {
    return hbox({
        text(" " + key + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
        text(" " + label + " ")
    });
}

Element help_overlay() {
    return vbox({
        text("KSM Control Center") | bold | color(Color::Cyan) | hcenter,
        separator(),
        text("Tab/Left/Right  Switch focus between panels"),
        text("Up/Down         Move in focused panel"),
        text("Enter           Select, edit, toggle, or run action"),
        text("F1/?            Toggle this help"),
        text("q/Esc           Back or exit"),
        separator(),
        text("Yellow modules may ask for sudo.") | color(Color::Yellow),
    }) | border | size(WIDTH, LESS_THAN, 64) | center;
}

Element edit_overlay(const TextEdit& edit) {
    return vbox({
        text(edit.title) | bold | color(Color::Cyan) | hcenter,
        separator(),
        text(edit.buffer.empty() ? " " : edit.buffer) | color(Color::White) | bgcolor(Color::Blue) | flex,
        separator(),
        text("Enter saves, Esc cancels, Backspace deletes.") | dim
    }) | borderStyled(Color::Cyan) | size(WIDTH, LESS_THAN, 72) | center;
}

Element picker_overlay(const PickerOverlay& picker) {
    Elements rows;
    const int pageSize = 14;
    int offset = 0;
    if (picker.index >= pageSize) offset = picker.index - pageSize + 1;
    const int end = std::min<int>(static_cast<int>(picker.items.size()), offset + pageSize);
    for (int i = offset; i < end; ++i) {
        rows.push_back(list_line(picker.items[i], i == picker.index, true, true));
    }
    if (picker.items.empty()) rows.push_back(text("No items.") | dim);

    return vbox({
        text(picker.title) | bold | color(Color::Cyan) | hcenter,
        separator(),
        vbox(std::move(rows)) | flex,
        separator(),
        text("Enter toggles, q/Esc saves and closes.") | dim
    }) | borderStyled(Color::Cyan) | size(WIDTH, LESS_THAN, 78) | size(HEIGHT, LESS_THAN, 22) | center;
}

Element notice_overlay(const NativePanel& panel) {
    const auto accent = panel.noticeOk ? Color::Green : Color::Red;
    return vbox({
        text(panel.noticeTitle) | bold | color(accent) | hcenter,
        separator(),
        paragraph(panel.noticeBody) | hcenter,
        separator(),
        text("Press Enter to return to the panel.") | dim | hcenter
    }) | borderStyled(accent) | size(WIDTH, LESS_THAN, 64) | center;
}

Element render_menu(
    const std::vector<Category>& items,
    int categoryIndex,
    int moduleIndex,
    const std::string& message,
    bool showHelp,
    FocusPane focus
) {
    const auto& category = items[categoryIndex];
    const auto& module = category.modules[moduleIndex];

    Elements categoryRows;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        categoryRows.push_back(category_line(items[i], i == categoryIndex, focus == FocusPane::Categories));
    }

    Elements moduleRows;
    for (int i = 0; i < static_cast<int>(category.modules.size()); ++i) {
        moduleRows.push_back(module_line(category.modules[i], i == moduleIndex, focus == FocusPane::Modules));
    }

    auto categoriesPanel = vbox(std::move(categoryRows)) |
        borderStyled(focus == FocusPane::Categories ? Color::Cyan : Color::Blue) |
        size(WIDTH, EQUAL, 26);

    auto modulesPanel = vbox(std::move(moduleRows)) |
        borderStyled(focus == FocusPane::Modules ? Color::Cyan : Color::Blue) |
        flex;

    auto detailsPanel = vbox({
        hbox({
            text(module.title) | bold | color(Color::Cyan),
            filler(),
            text(module.needsRoot ? "sudo/root" : "normal user") |
                color(module.needsRoot ? Color::Yellow : Color::Green)
        }),
        separator(),
        hbox({text("Panel path  ") | dim, text("sudo ksm > " + category.title + " > " + module.title) | color(Color::Cyan)}),
        hbox({text("Description ") | dim, paragraph(module.description) | flex}),
        hbox({text("Category    ") | dim, text(category.title)}),
        separator(),
        paragraph("Enter opens the native KSM panel for this function. Everything is reached through sudo ksm.") | dim
    }) | borderStyled(Color::Blue);

    auto status = message.empty() ? text("Ready") | dim : text(message) | color(Color::Yellow);
    auto footer = hbox({
        action_button("Enter", focus == FocusPane::Categories ? "Choose category" : "Open panel"),
        text("  "),
        action_button("Tab/<-/->", "Switch focus"),
        text("  "),
        action_button("Up/Down", focus == FocusPane::Categories ? "Category" : "Module"),
        text("  "),
        action_button("F1", "Help"),
        text("  "),
        action_button("q", "Exit"),
        filler(),
        status
    }) | borderStyled(Color::Blue);

    auto document = vbox({
        text("KASTIUSZ SYSTEM MANAGER") | bold | color(Color::Cyan) | hcenter,
        text(std::string("FTXUI Control Center - focus: ") +
             (focus == FocusPane::Categories ? "Categories" : "Modules")) | dim | hcenter,
        separator(),
        hbox({
            vbox({text(" Categories ") | bold | color(Color::Cyan), categoriesPanel | flex}),
            text("  "),
            vbox({text(" Modules: " + category.title + " ") | bold | color(Color::Cyan), modulesPanel | flex}) | flex
        }) | flex,
        detailsPanel,
        footer
    }) | borderStyled(Color::Cyan);

    if (showHelp) return dbox({document, help_overlay()});
    return document;
}

Element render_native_panel(
    const NativePanel& panel,
    bool showHelp,
    const TextEdit& edit,
    const PickerOverlay& picker
) {
    Elements rowElements;
    for (int i = 0; i < static_cast<int>(panel.rows.size()); ++i) {
        rowElements.push_back(row_line(panel.rows[i], i == panel.rowIndex, panel.focus == PanelFocus::Rows));
    }

    auto rowsPanel = vbox(std::move(rowElements)) |
        borderStyled(panel.focus == PanelFocus::Rows ? Color::Cyan : Color::Blue) |
        flex;

    Elements details;
    if (!panel.selectedKey.empty()) {
        details.push_back(hbox({text("Selected ") | dim, text(panel.selectedKey) | color(Color::Cyan)}));
        details.push_back(separator());
    }
    for (const auto& line : panel.detailLines) details.push_back(paragraph(line));
    if (details.empty()) details.push_back(paragraph(panel.subtitle));
    auto detailPanel = vbox(std::move(details)) | borderStyled(Color::Blue);

    Element mainArea;
    if (!panel.list.empty()) {
        Elements listRows;
        const int pageSize = 15;
        int offset = 0;
        if (panel.listIndex >= pageSize) offset = panel.listIndex - pageSize + 1;
        const int end = std::min<int>(static_cast<int>(panel.list.size()), offset + pageSize);
        for (int i = offset; i < end; ++i) {
            listRows.push_back(list_line(panel.list[i], i == panel.listIndex, panel.focus == PanelFocus::List, panel.multiList));
        }
        auto listPanel = vbox(std::move(listRows)) |
            borderStyled(panel.focus == PanelFocus::List ? Color::Cyan : Color::Blue) |
            flex;

        mainArea = hbox({
            vbox({text(" Items ") | bold | color(Color::Cyan), listPanel | flex}) | flex,
            text("  "),
            vbox({text(" Controls ") | bold | color(Color::Cyan), rowsPanel | flex}) | flex
        }) | flex;
    } else {
        mainArea = rowsPanel | flex;
    }

    auto status = panel.message.empty() ? text("Ready") | dim : text(panel.message) | color(Color::Yellow);
    auto footer = hbox({
        action_button("Enter", "Select/Edit/Run"),
        text("  "),
        action_button("Tab", "Switch pane"),
        text("  "),
        action_button("Up/Down", "Move"),
        text("  "),
        action_button("Esc/q", "Back"),
        filler(),
        status
    }) | borderStyled(Color::Blue);

    auto document = vbox({
        text(panel.title) | bold | color(Color::Cyan) | hcenter,
        text(panel.subtitle) | dim | hcenter,
        separator(),
        mainArea,
        detailPanel,
        footer
    }) | borderStyled(Color::Cyan);

    if (showHelp) document = dbox({document, help_overlay()});
    if (!panel.noticeTitle.empty()) document = dbox({document, notice_overlay(panel)});
    if (picker.active) document = dbox({document, picker_overlay(picker)});
    if (edit.active) document = dbox({document, edit_overlay(edit)});
    return document;
}

void clamp_module(const std::vector<Category>& items, int categoryIndex, int& moduleIndex) {
    const int maxIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
    if (moduleIndex > maxIndex) moduleIndex = maxIndex;
    if (moduleIndex < 0) moduleIndex = 0;
}

void cycle_choice(Row& row, int delta) {
    if (row.choices.empty()) return;
    auto it = std::find(row.choices.begin(), row.choices.end(), row.value);
    int index = it == row.choices.end() ? 0 : static_cast<int>(it - row.choices.begin());
    index += delta;
    if (index < 0) index = static_cast<int>(row.choices.size()) - 1;
    if (index >= static_cast<int>(row.choices.size())) index = 0;
    row.value = row.choices[index];
}

void toggle_current_list_item(NativePanel& panel) {
    if (panel.list.empty()) return;
    ListItem& item = panel.list[panel.listIndex];
    if (panel.multiList) {
        item.selected = !item.selected;
        panel.message = item.selected ? "Selected " + item.label + "." : "Unselected " + item.label + ".";
        return;
    }

    panel.selectedKey = item.key;
    if (panel.kind == ModuleKind::ZpmExtension) {
        panel.message = "Selected ZPM function: " + item.label + ".";
        return;
    }
    panel.message = "Selected " + item.label + ".";
    if (panel.kind == ModuleKind::UserMod) load_user_into_panel(panel, item.key);
    if (panel.kind == ModuleKind::GroupMod) load_group_into_panel(panel, item.key);
    if (panel.kind == ModuleKind::Network) load_network_current(panel);
    if (panel.kind == ModuleKind::DhcpServer) {
        set_row_value(panel, "interface", item.key);
        panel.message = "DHCP interface set to " + item.key + ".";
    }
}

void handle_local_action(NativePanel& panel, const std::string& id) {
    if (id == "refresh_sysinfo") {
        refresh_sysinfo(panel);
        return;
    }
    if (id == "refresh_home") {
        const std::string page = row_value(panel, "page");
        panel.detailLines.clear();
        if (page == "Commands") {
            panel.detailLines = {
                "Public command: sudo ksm.",
                "Users, groups, network, services, SSH, firewall and update live in this panel.",
                "Legacy helper binaries are internal only."
            };
        } else if (page == "Panel controls") {
            panel.detailLines = {
                "Categories and modules have separate focus.",
                "Inside modules, item lists and controls have separate focus.",
                "All module work launched from kcontrol stays in the panel look."
            };
        } else {
            panel.detailLines = {
                "Kastiusz System Manager is controlled from this FTXUI panel.",
                "Public entrypoint: sudo ksm.",
                "The panel path gives every function a unified YaST-style screen."
            };
        }
        panel.message = "Home panel refreshed.";
        return;
    }
    if (id == "refresh_users") {
        panel.list = read_users(row_checked(panel, "show_system"), true);
        panel.listIndex = 0;
        panel.message = "Users reloaded.";
        return;
    }
    if (id == "refresh_groups") {
        panel.list = read_groups(row_checked(panel, "show_system"), true);
        panel.listIndex = 0;
        panel.message = "Groups reloaded.";
        return;
    }
    if (id == "select_keyword") {
        const std::string keyword = trim(row_value(panel, "keyword"));
        int count = 0;
        if (!keyword.empty()) {
            const std::string needle = lower(keyword);
            for (auto& item : panel.list) {
                if (lower(item.label).find(needle) != std::string::npos) {
                    item.selected = true;
                    ++count;
                }
            }
        }
        panel.message = count == 0 ? "No keyword matches." : "Selected keyword matches: " + std::to_string(count) + ".";
        return;
    }
    if (id == "select_name") {
        const std::string name = trim(row_value(panel, "name"));
        int count = 0;
        if (!name.empty()) {
            const std::string needle = lower(name);
            for (auto& item : panel.list) {
                if (lower(item.label).find(needle) != std::string::npos) {
                    item.selected = true;
                    ++count;
                }
            }
        }
        panel.message = count == 0 ? "No name matches." : "Selected name matches: " + std::to_string(count) + ".";
        return;
    }
    if (id == "reload_services") {
        panel.list = read_services();
        panel.listIndex = 0;
        panel.message = "Services reloaded.";
        return;
    }
    if (id == "load_perm") {
        load_perm_current(panel);
        return;
    }
    if (id == "load_network") {
        load_network_current(panel);
        return;
    }
    if (id == "load_ssh") {
        load_ssh_config(panel);
        return;
    }
}

bool is_local_action(const std::string& id) {
    return id == "refresh_sysinfo" || id == "refresh_home" || id == "refresh_users" ||
           id == "refresh_groups" || id == "select_keyword" || id == "select_name" || id == "reload_services" ||
           id == "load_perm" || id == "load_network" || id == "load_ssh";
}

bool valid_username(const std::string& username) {
    if (username.empty()) return false;
    for (char ch : username) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::islower(c) || std::isdigit(c) || ch == '_' || ch == '-') continue;
        return false;
    }
    return true;
}

std::string add_group(std::string groups, const std::string& group) {
    auto values = split_csv(groups);
    if (!contains_value(values, group)) values.push_back(group);
    return join_csv(values);
}

void execute_useradd(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;

    const int count = parse_positive(row_value(panel, "amount"), 1);
    std::string usernameTemplate = lower(trim(row_value(panel, "username")));
    const std::string passwordTemplate = row_value(panel, "password");
    const std::string fullNameTemplate = row_value(panel, "full_name");
    const std::string shell = trim(row_value(panel, "shell"));
    const std::string homeTemplate = trim(row_value(panel, "home"));
    std::string groups = trim(row_value(panel, "groups"));

    if (row_checked(panel, "sudo")) groups = add_group(groups, "sudo");
    if (usernameTemplate.empty()) {
        panel.message = "Username template is required.";
        return;
    }
    if (count > 1 && usernameTemplate.find("$i") == std::string::npos) {
        usernameTemplate += "$i";
    }

    int failures = 0;
    for (int i = 1; i <= count; ++i) {
        std::string username = lower(replace_index(usernameTemplate, i));
        if (!valid_username(username)) {
            ++failures;
            continue;
        }

        std::vector<std::string> args = {"useradd"};
        args.push_back(row_checked(panel, "create_home") ? "-m" : "-M");
        if (!shell.empty()) {
            args.push_back("-s");
            args.push_back(shell);
        }
        if (!homeTemplate.empty()) {
            args.push_back("-d");
            args.push_back(replace_index(homeTemplate, i));
        }
        const std::string fullName = replace_index(fullNameTemplate, i);
        if (!fullName.empty()) {
            args.push_back("-c");
            args.push_back(fullName);
        }
        const std::string userGroups = replace_index(groups, i);
        if (!userGroups.empty()) {
            args.push_back("-G");
            args.push_back(userGroups);
        }
        args.push_back(username);

        CommandResult result = run_command(args, true);
        if (result.code != 0) {
            ++failures;
            continue;
        }
        const std::string password = replace_index(passwordTemplate, i);
        if (!password.empty()) {
            result = run_command({"chpasswd"}, true, username + ":" + password + "\n");
            if (result.code != 0) ++failures;
        }
    }

    panel.message = failures == 0
        ? "Created users: " + std::to_string(count) + "."
        : "Done with failures: " + std::to_string(failures) + ".";
}

void execute_userdel(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    const auto users = selected_keys(panel);
    if (users.empty()) {
        panel.message = "Select at least one user.";
        return;
    }

    int failures = 0;
    for (const auto& user : users) {
        std::vector<std::string> args = {"userdel"};
        if (row_checked(panel, "remove_home")) args.push_back("-r");
        if (row_checked(panel, "force")) args.push_back("-f");
        args.push_back(user);
        if (run_command(args, true).code != 0) ++failures;
    }
    panel.list = read_users(row_checked(panel, "show_system"), true);
    panel.listIndex = 0;
    panel.message = failures == 0 ? "Deleted selected users." : "User delete failures: " + std::to_string(failures) + ".";
}

void execute_usermod(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    if (panel.selectedKey.empty()) {
        panel.message = "Select a user first.";
        return;
    }

    int failures = 0;
    std::vector<std::string> args = {"usermod"};
    const std::string newLogin = trim(row_value(panel, "new_login"));
    if (!newLogin.empty()) {
        args.push_back("-l");
        args.push_back(lower(newLogin));
    }
    const std::string fullName = row_value(panel, "full_name");
    if (!fullName.empty()) {
        args.push_back("-c");
        args.push_back(fullName);
    }
    const std::string home = trim(row_value(panel, "home"));
    if (!home.empty()) {
        args.push_back("-d");
        args.push_back(home);
        if (row_checked(panel, "move_home")) args.push_back("-m");
    }
    const std::string shell = trim(row_value(panel, "shell"));
    if (!shell.empty()) {
        args.push_back("-s");
        args.push_back(shell);
    }
    const std::string groups = trim(row_value(panel, "groups"));
    if (!groups.empty()) {
        args.push_back(row_checked(panel, "append_groups") ? "-aG" : "-G");
        args.push_back(groups);
    }
    args.push_back(panel.selectedKey);

    if (args.size() > 2 && run_command(args, true).code != 0) ++failures;
    if (row_checked(panel, "lock") && run_command({"passwd", "-l", panel.selectedKey}, true).code != 0) ++failures;
    if (row_checked(panel, "unlock") && run_command({"passwd", "-u", panel.selectedKey}, true).code != 0) ++failures;

    panel.list = read_users(row_checked(panel, "show_system"), false);
    panel.message = failures == 0 ? "User changes applied." : "Usermod failures: " + std::to_string(failures) + ".";
}

void execute_groupadd(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    const int count = parse_positive(row_value(panel, "amount"), 1);
    std::string groupTemplate = lower(trim(row_value(panel, "name")));
    const std::string gidText = trim(row_value(panel, "gid"));
    const std::string members = trim(row_value(panel, "members"));

    if (groupTemplate.empty()) {
        panel.message = "Group template is required.";
        return;
    }
    if (count > 1 && groupTemplate.find("$i") == std::string::npos) {
        groupTemplate += "$i";
    }

    int gidStart = 0;
    if (!gidText.empty()) gidStart = parse_positive(gidText, 0);

    int failures = 0;
    for (int i = 1; i <= count; ++i) {
        const std::string groupName = replace_index(groupTemplate, i);
        std::vector<std::string> args = {"groupadd"};
        if (row_checked(panel, "system")) args.push_back("-r");
        if (gidStart > 0) {
            args.push_back("-g");
            args.push_back(std::to_string(gidStart + i - 1));
        }
        args.push_back(groupName);
        if (run_command(args, true).code != 0) {
            ++failures;
            continue;
        }
        if (!members.empty() && run_command({"gpasswd", "-M", members, groupName}, true).code != 0) {
            ++failures;
        }
    }

    panel.message = failures == 0
        ? "Created groups: " + std::to_string(count) + "."
        : "Done with group failures: " + std::to_string(failures) + ".";
}

void execute_groupdel(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    const auto groups = selected_keys(panel);
    if (groups.empty()) {
        panel.message = "Select at least one group.";
        return;
    }

    int failures = 0;
    for (const auto& group : groups) {
        std::vector<std::string> args = {"groupdel"};
        if (row_checked(panel, "force")) args.push_back("-f");
        args.push_back(group);
        if (run_command(args, true).code != 0) ++failures;
    }
    panel.list = read_groups(row_checked(panel, "show_system"), true);
    panel.listIndex = 0;
    panel.message = failures == 0 ? "Deleted selected groups." : "Group delete failures: " + std::to_string(failures) + ".";
}

void execute_groupmod(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    if (panel.selectedKey.empty()) {
        panel.message = "Select a group first.";
        return;
    }

    int failures = 0;
    std::vector<std::string> args = {"groupmod"};
    const std::string newName = lower(trim(row_value(panel, "new_name")));
    const std::string gid = trim(row_value(panel, "gid"));
    if (!newName.empty()) {
        args.push_back("-n");
        args.push_back(newName);
    }
    if (!gid.empty()) {
        args.push_back("-g");
        args.push_back(gid);
    }
    args.push_back(panel.selectedKey);
    if (args.size() > 2 && run_command(args, true).code != 0) ++failures;

    const std::string finalName = newName.empty() ? panel.selectedKey : newName;
    const std::string members = trim(row_value(panel, "members"));
    if (!members.empty() && run_command({"gpasswd", "-M", members, finalName}, true).code != 0) ++failures;

    panel.list = read_groups(row_checked(panel, "show_system"), false);
    panel.message = failures == 0 ? "Group changes applied." : "Groupmod failures: " + std::to_string(failures) + ".";
}

void execute_service(NativePanel& panel, const std::string& action) {
    if (panel.selectedKey.empty()) {
        panel.message = "Select a service first.";
        return;
    }
    std::string systemctlAction = action;
    const std::string suffix = "_service";
    if (systemctlAction.size() > suffix.size() &&
        systemctlAction.substr(systemctlAction.size() - suffix.size()) == suffix) {
        systemctlAction = systemctlAction.substr(0, systemctlAction.size() - suffix.size());
    }
    if (!ensure_root_auth(panel.message)) return;
    const CommandResult result = run_command({"systemctl", systemctlAction, panel.selectedKey}, true);
    panel.list = read_services();
    panel.message = result.code == 0 ? "systemctl " + systemctlAction + " done." : "systemctl failed: " + std::to_string(result.code) + ".";
}

void execute_perm(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    const std::string path = trim(row_value(panel, "path"));
    if (path.empty()) {
        panel.message = "Path is required.";
        return;
    }

    int failures = 0;
    const std::string owner = trim(row_value(panel, "owner"));
    const std::string group = trim(row_value(panel, "group"));
    if (!owner.empty() || !group.empty()) {
        std::vector<std::string> args = {"chown"};
        if (row_checked(panel, "recursive")) args.push_back("-R");
        args.push_back(owner + ":" + group);
        args.push_back(path);
        if (run_command(args, true).code != 0) ++failures;
    }
    const std::string mode = trim(row_value(panel, "mode"));
    if (!mode.empty()) {
        std::vector<std::string> args = {"chmod"};
        if (row_checked(panel, "recursive")) args.push_back("-R");
        args.push_back(mode);
        args.push_back(path);
        if (run_command(args, true).code != 0) ++failures;
    }
    panel.message = failures == 0 ? "Permissions applied." : "Permission failures: " + std::to_string(failures) + ".";
    load_perm_current(panel);
}

void execute_network(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    if (panel.selectedKey.empty()) {
        panel.message = "Select an interface first.";
        return;
    }
    const std::string iface = panel.selectedKey;
    int failures = 0;
    if (run_command({"ip", "link", "set", iface, row_checked(panel, "enable") ? "up" : "down"}, true).code != 0) ++failures;
    if (row_checked(panel, "flush")) {
        run_command({"ip", "addr", "flush", "dev", iface}, true);
    }
    if (!row_checked(panel, "dhcp")) {
        const std::string address = trim(row_value(panel, "address"));
        const std::string gateway = trim(row_value(panel, "gateway"));
        if (!address.empty() && run_command({"ip", "addr", "add", address, "dev", iface}, true).code != 0) ++failures;
        if (!gateway.empty() && run_command({"ip", "route", "replace", "default", "via", gateway, "dev", iface}, true).code != 0) ++failures;
    }
    const std::string dns = trim(row_value(panel, "dns"));
    if (!dns.empty()) {
        if (command_exists("resolvectl")) {
            std::vector<std::string> args = {"resolvectl", "dns", iface};
            for (const auto& server : split_csv(dns)) args.push_back(server);
            if (run_command(args, true).code != 0) ++failures;
        } else {
            std::string content;
            for (const auto& server : split_csv(dns)) content += "nameserver " + server + "\n";
            const fs::path temp = fs::path("/tmp") / ("ksm-resolv-" + std::to_string(getpid()) + ".conf");
            {
                std::ofstream file(temp);
                file << content;
            }
            if (run_command({"cp", temp.string(), "/etc/resolv.conf"}, true).code != 0) ++failures;
            std::error_code ec;
            fs::remove(temp, ec);
        }
    }
    panel.message = failures == 0 ? "Runtime network config applied." : "Network failures: " + std::to_string(failures) + ".";
}

std::string set_config_value(const std::string& input, const std::string& key, const std::string& value) {
    std::stringstream in(input);
    std::string out;
    std::string line;
    bool replaced = false;
    while (std::getline(in, line)) {
        std::string clean = trim(line);
        if (!clean.empty() && clean[0] == '#') clean = trim(clean.substr(1));
        std::istringstream ss(clean);
        std::string currentKey;
        ss >> currentKey;
        if (lower(currentKey) == lower(key)) {
            out += key + " " + value + "\n";
            replaced = true;
        } else {
            out += line + "\n";
        }
    }
    if (!replaced) out += key + " " + value + "\n";
    return out;
}

void execute_ssh(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    std::string content;
    {
        std::ifstream file("/etc/ssh/sshd_config");
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            content = buffer.str();
        }
    }
    content = set_config_value(content, "Port", row_value(panel, "port"));
    content = set_config_value(content, "PermitRootLogin", row_value(panel, "root"));
    content = set_config_value(content, "PasswordAuthentication", row_value(panel, "password"));
    content = set_config_value(content, "PubkeyAuthentication", row_value(panel, "pubkey"));
    content = set_config_value(content, "X11Forwarding", row_value(panel, "x11"));

    const fs::path temp = fs::path("/tmp") / ("ksm-sshd-" + std::to_string(getpid()) + ".conf");
    {
        std::ofstream file(temp);
        file << content;
    }
    int failures = 0;
    run_command({"cp", "/etc/ssh/sshd_config", "/etc/ssh/sshd_config.ksm.bak"}, true);
    if (run_command({"cp", temp.string(), "/etc/ssh/sshd_config"}, true).code != 0) ++failures;
    std::error_code ec;
    fs::remove(temp, ec);
    if (row_checked(panel, "restart")) {
        CommandResult result = run_command({"systemctl", "restart", "ssh"}, true);
        if (result.code != 0) result = run_command({"systemctl", "restart", "sshd"}, true);
        if (result.code != 0) ++failures;
    }
    panel.message = failures == 0 ? "sshd_config applied." : "SSH apply failures: " + std::to_string(failures) + ".";
}

std::string firewall_backend(const NativePanel& panel) {
    std::string backend = row_value(panel, "backend");
    if (backend == "auto") {
        if (command_exists("ufw")) return "ufw";
        if (command_exists("firewall-cmd")) return "firewalld";
        return "none";
    }
    return backend;
}

void execute_firewall(NativePanel& panel, const std::string& actionId) {
    if (!ensure_root_auth(panel.message)) return;
    const std::string backend = firewall_backend(panel);
    if (backend == "none") {
        panel.message = "No ufw or firewalld found.";
        return;
    }

    int code = 1;
    if (actionId == "enable_firewall") {
        if (backend == "ufw") code = run_command({"ufw", "enable"}, true).code;
        else code = run_command({"systemctl", "enable", "--now", "firewalld"}, true).code;
        panel.message = code == 0 ? "Firewall enabled." : "Enable firewall failed.";
        return;
    }

    const std::string port = trim(row_value(panel, "port"));
    const std::string protocol = row_value(panel, "protocol");
    const std::string action = row_value(panel, "action");
    if (port.empty()) {
        panel.message = "Port is required.";
        return;
    }
    if (backend == "ufw") {
        if (action == "delete") code = run_command({"ufw", "delete", "allow", port + "/" + protocol}, true).code;
        else code = run_command({"ufw", action, port + "/" + protocol}, true).code;
    } else {
        const std::string flag = action == "delete" ? "--remove-port=" : "--add-port=";
        code = run_command({"firewall-cmd", "--permanent", flag + port + "/" + protocol}, true).code;
        if (code == 0) run_command({"firewall-cmd", "--reload"}, true);
    }
    panel.message = code == 0 ? "Firewall rule applied." : "Firewall command failed.";
}

std::string remove_marked_block(const std::string& content, const std::string& marker) {
    const std::string begin = "# KSM-BEGIN " + marker;
    const std::string end = "# KSM-END " + marker;
    std::string output = content;
    size_t start = output.find(begin);
    while (start != std::string::npos) {
        size_t finish = output.find(end, start);
        if (finish == std::string::npos) break;
        finish += end.size();
        if (finish < output.size() && output[finish] == '\n') ++finish;
        output.erase(start, finish - start);
        start = output.find(begin);
    }
    return output;
}

std::string on_off(bool value) {
    return value ? "yes" : "no";
}

void execute_power(NativePanel& panel, const std::string& actionId) {
    if (!ensure_root_auth(panel.message)) return;
    int code = 1;
    if (actionId == "reboot_now") {
        code = run_command({"systemctl", "reboot"}, true).code;
        panel.message = code == 0 ? "Reboot requested." : "Reboot failed.";
    } else if (actionId == "shutdown_now") {
        code = run_command({"systemctl", "poweroff"}, true).code;
        panel.message = code == 0 ? "Shutdown requested." : "Shutdown failed.";
    } else if (actionId == "shutdown_later") {
        int delay = 0;
        try {
            delay = std::max(0, std::stoi(trim(row_value(panel, "delay"))));
        } catch (...) {
            delay = 0;
        }
        const std::string when = delay <= 0 ? "now" : "+" + std::to_string(delay);
        code = run_command({"shutdown", "-h", when}, true).code;
        panel.message = code == 0 ? "Shutdown scheduled." : "Schedule shutdown failed.";
    } else if (actionId == "cancel_shutdown") {
        code = run_command({"shutdown", "-c"}, true).code;
        panel.message = code == 0 ? "Scheduled shutdown cancelled." : "Cancel shutdown failed.";
    }
}

void execute_install_extension(NativePanel& panel, const std::string& feature, const std::vector<std::string>& services) {
    if (!ensure_root_auth(panel.message)) return;
    const CommandResult result = install_extension_packages(feature);
    refresh_server_status(panel, "status", services);
    panel.message = result.code == 0
        ? "Installed " + feature + " extension."
        : "Install failed: " + fit_text(strip_ansi(result.output), 80);
}

void execute_service_panel_action(NativePanel& panel, const std::vector<std::string>& services, const std::string& actionId, const std::string& label) {
    if (!ensure_root_auth(panel.message)) return;
    bool ok = false;
    if (actionId.rfind("restart_", 0) == 0) ok = service_action(services, {"restart"});
    else if (actionId.rfind("enable_", 0) == 0) ok = service_action(services, {"enable", "--now"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? label + " service action completed." : label + " service action failed.";
}

void execute_samba(NativePanel& panel, const std::string& actionId) {
    const std::vector<std::string> services = {"smbd", "samba"};
    if (actionId == "install_samba") {
        execute_install_extension(panel, "samba", services);
        return;
    }
    if (actionId == "restart_samba" || actionId == "enable_samba") {
        execute_service_panel_action(panel, services, actionId, "Samba");
        return;
    }
    if (!ensure_root_auth(panel.message)) return;

    const std::string share = trim(row_value(panel, "share"));
    const std::string path = trim(row_value(panel, "path"));
    if (share.empty() || path.empty()) {
        panel.message = "Share name and path are required.";
        return;
    }
    if (row_checked(panel, "create_path")) run_command({"mkdir", "-p", path}, true);

    std::string content = read_file_text("/etc/samba/smb.conf");
    content = remove_marked_block(content, "samba:" + share);
    content += "\n# KSM-BEGIN samba:" + share + "\n";
    content += "[" + share + "]\n";
    content += "   path = " + path + "\n";
    content += "   writable = " + on_off(row_checked(panel, "writable")) + "\n";
    content += "   guest ok = " + on_off(row_checked(panel, "guest")) + "\n";
    content += "   browseable = " + on_off(row_checked(panel, "browseable")) + "\n";
    content += "   create mask = 0664\n";
    content += "   directory mask = 0775\n";
    content += "# KSM-END samba:" + share + "\n";

    const bool ok = write_root_file("/etc/samba/smb.conf", content);
    if (ok) service_action(services, {"restart"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? "Samba share applied." : "Writing smb.conf failed.";
}

void execute_apache(NativePanel& panel, const std::string& actionId) {
    const std::vector<std::string> services = {"apache2", "httpd"};
    if (actionId == "install_apache") {
        execute_install_extension(panel, "apache", services);
        return;
    }
    if (actionId == "restart_apache" || actionId == "enable_apache") {
        execute_service_panel_action(panel, services, actionId, "Apache");
        return;
    }
    if (!ensure_root_auth(panel.message)) return;

    const std::string port = trim(row_value(panel, "port"));
    const std::string root = trim(row_value(panel, "document_root"));
    const std::string serverName = trim(row_value(panel, "server_name"));
    if (port.empty() || root.empty()) {
        panel.message = "Port and DocumentRoot are required.";
        return;
    }

    run_command({"mkdir", "-p", root}, true);
    if (row_checked(panel, "create_index")) {
        const std::string title = trim(row_value(panel, "index_title")).empty() ? "KSM Apache" : trim(row_value(panel, "index_title"));
        const std::string index = "<!doctype html><title>" + title + "</title><h1>" + title + "</h1>\n";
        write_root_file(fs::path(root) / "index.html", index, false);
    }

    std::string config;
    config += "<VirtualHost *:" + port + ">\n";
    if (!serverName.empty() && serverName != "_default_") config += "    ServerName " + serverName + "\n";
    config += "    DocumentRoot " + root + "\n";
    config += "    <Directory \"" + root + "\">\n";
    config += "        Options Indexes FollowSymLinks\n";
    config += "        AllowOverride None\n";
    config += "        Require all granted\n";
    config += "    </Directory>\n";
    config += "</VirtualHost>\n";

    const fs::path apachePath = fs::exists("/etc/apache2") ? fs::path("/etc/apache2/sites-available/ksm.conf") : fs::path("/etc/httpd/conf.d/ksm.conf");
    const bool ok = write_root_file(apachePath, config);
    if (ok && apachePath.string().find("/etc/apache2/") == 0 && command_exists("a2ensite")) {
        run_command({"a2ensite", "ksm.conf"}, true);
    }
    if (ok) service_action(services, {"restart"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? "Apache virtual host applied." : "Writing Apache config failed.";
}

void execute_dns(NativePanel& panel, const std::string& actionId) {
    const std::vector<std::string> services = {"bind9", "named"};
    if (actionId == "install_dns") {
        execute_install_extension(panel, "dns", services);
        return;
    }
    if (actionId == "restart_dns" || actionId == "enable_dns") {
        execute_service_panel_action(panel, services, actionId, "DNS");
        return;
    }
    if (!ensure_root_auth(panel.message)) return;

    const auto forwarders = split_csv(row_value(panel, "forwarders"));
    if (forwarders.empty()) {
        panel.message = "At least one forwarder is required.";
        return;
    }
    const bool bindDir = fs::exists("/etc/bind");
    const fs::path configPath = bindDir ? fs::path("/etc/bind/named.conf.options") : fs::path("/etc/named.conf");
    const std::string directory = fs::exists("/var/named") ? "/var/named" : "/var/cache/bind";

    std::string config;
    config += "options {\n";
    config += "    directory \"" + directory + "\";\n";
    config += (row_checked(panel, "listen_all") ? "    listen-on port 53 { any; };\n" : "    listen-on port 53 { 127.0.0.1; };\n");
    config += "    allow-query { any; };\n";
    config += "    recursion yes;\n";
    config += "    forwarders { ";
    for (const auto& server : forwarders) config += server + "; ";
    config += "};\n";
    config += "    dnssec-validation auto;\n";
    config += "};\n";

    const bool ok = write_root_file(configPath, config);
    if (ok) service_action(services, {"restart"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? "DNS cache config applied." : "Writing DNS config failed.";
}

void execute_dhcp(NativePanel& panel, const std::string& actionId) {
    const std::vector<std::string> services = {"isc-dhcp-server", "dhcpd"};
    if (actionId == "install_dhcp") {
        execute_install_extension(panel, "dhcp", services);
        return;
    }
    if (actionId == "restart_dhcp" || actionId == "enable_dhcp") {
        execute_service_panel_action(panel, services, actionId, "DHCP");
        return;
    }
    if (!ensure_root_auth(panel.message)) return;

    const std::string iface = trim(row_value(panel, "interface"));
    const std::string subnet = trim(row_value(panel, "subnet"));
    const std::string netmask = trim(row_value(panel, "netmask"));
    const std::string start = trim(row_value(panel, "range_start"));
    const std::string end = trim(row_value(panel, "range_end"));
    const std::string router = trim(row_value(panel, "router"));
    if (iface.empty() || subnet.empty() || netmask.empty() || start.empty() || end.empty() || router.empty()) {
        panel.message = "Interface, subnet, netmask, range, and router are required.";
        return;
    }

    std::string config;
    config += "default-lease-time 600;\n";
    config += "max-lease-time 7200;\n";
    config += "authoritative;\n\n";
    config += "subnet " + subnet + " netmask " + netmask + " {\n";
    config += "  range " + start + " " + end + ";\n";
    config += "  option routers " + router + ";\n";
    if (!trim(row_value(panel, "dns")).empty()) config += "  option domain-name-servers " + row_value(panel, "dns") + ";\n";
    config += "}\n";

    bool ok = write_root_file("/etc/dhcp/dhcpd.conf", config);
    if (fs::exists("/etc/default")) {
        ok = write_root_file("/etc/default/isc-dhcp-server", "INTERFACESv4=\"" + iface + "\"\n", true) && ok;
    }
    if (fs::exists("/etc/sysconfig")) {
        ok = write_root_file("/etc/sysconfig/dhcpd", "DHCPDARGS=\"" + iface + "\"\n", true) && ok;
    }
    if (ok) service_action(services, {"restart"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? "DHCP config applied." : "Writing DHCP config failed.";
}

void execute_ftp(NativePanel& panel, const std::string& actionId) {
    const std::vector<std::string> services = {"vsftpd"};
    if (actionId == "install_ftp") {
        execute_install_extension(panel, "ftp", services);
        return;
    }
    if (actionId == "restart_ftp" || actionId == "enable_ftp") {
        execute_service_panel_action(panel, services, actionId, "FTP");
        return;
    }
    if (!ensure_root_auth(panel.message)) return;

    std::string config;
    config += "listen=NO\n";
    config += "listen_ipv6=YES\n";
    config += "anonymous_enable=" + std::string(row_checked(panel, "anonymous") ? "YES" : "NO") + "\n";
    config += "local_enable=" + std::string(row_checked(panel, "local_users") ? "YES" : "NO") + "\n";
    config += "write_enable=" + std::string(row_checked(panel, "writable") ? "YES" : "NO") + "\n";
    config += "dirmessage_enable=YES\n";
    config += "use_localtime=YES\n";
    config += "xferlog_enable=YES\n";
    config += "secure_chroot_dir=/var/run/vsftpd/empty\n";
    const std::string banner = trim(row_value(panel, "banner"));
    if (!banner.empty()) config += "ftpd_banner=" + banner + "\n";

    const bool ok = write_root_file("/etc/vsftpd.conf", config);
    if (ok) service_action(services, {"restart"});
    refresh_server_status(panel, "status", services);
    panel.message = ok ? "FTP config applied." : "Writing vsftpd config failed.";
}

void refresh_zpm_status(NativePanel& panel) {
    set_row_value(panel, "status", zpm_installed() ? "installed" : "not installed");
}

void set_terminal_output(NativePanel& panel, const std::string& title, const std::string& output) {
    panel.list.clear();

    ListItem header;
    header.key = "terminal-header";
    header.label = title;
    header.detail = output.empty() ? "(no output)" : "";
    panel.list.push_back(header);

    if (!output.empty()) {
        std::stringstream stream(strip_ansi(output));
        std::string line;
        int index = 1;
        while (std::getline(stream, line)) {
            ListItem item;
            item.key = "terminal-" + std::to_string(index);
            item.label = std::to_string(index);
            item.detail = line.empty() ? " " : line;
            panel.list.push_back(item);
            ++index;
        }
    }

    panel.listIndex = 0;
}

void execute_install_zpm(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    if (!command_exists("curl")) {
        panel.message = "Missing curl. Install curl first.";
        panel.noticeTitle = "ZPM INSTALL FAILED";
        panel.noticeBody = "curl is required to download the ZPM installer.";
        panel.noticeOk = false;
        return;
    }
    const std::string script =
        "bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)\"";
    CommandResult result = run_command_with_progress(
        {"bash", "-lc", script},
        true,
        "y\ny\n",
        "ZPM INSTALL",
        "ZPM installation complete.",
        "ZPM installation failed. Check /tmp/ZPM_INETINSTALL.log.",
        {
            "Authorize",
            "Check system",
            "Dependencies",
            "Fetch release",
            "Download",
            "Extract",
            "Install files",
            "Compile",
            "Link commands",
            "Finish"
        }
    );

    refresh_zpm_status(panel);
    set_terminal_output(panel, "ZPM installer output", result.output);

    if (result.code == 0) {
        const bool installed = zpm_installed();
        panel.noticeTitle = installed ? "ZPM INSTALLED" : "ZPM INSTALL CHECK FAILED";
        panel.noticeBody = installed
            ? "ZPM extension installed. Go back to the menu to open the new ZPM tab."
            : "Installer finished, but /opt/ZPM was not found.";
        panel.noticeOk = installed;
        panel.message = installed ? "ZPM extension installed. ZPM tab is available." : "Installer finished, /opt/ZPM missing.";
    } else {
        panel.noticeTitle = "ZPM INSTALL FAILED";
        panel.noticeBody = "Installer exited with code " + std::to_string(result.code) + ".";
        panel.noticeOk = false;
        panel.message = "ZPM install failed.";
    }
}

std::vector<std::string> build_zpm_args(NativePanel& panel) {
    const std::string command = panel.selectedKey.empty() ? "zinst" : panel.selectedKey;
    std::vector<std::string> args = {zpm_executable(command)};

    auto addPackages = [&](const std::string& rowId) {
        for (const auto& package : split_words(row_value(panel, rowId))) args.push_back(package);
    };
    auto addSource = [&]() {
        const std::string source = row_value(panel, "source");
        if (source == "native") args.push_back("-n");
        else if (source == "flatpak") args.push_back("-f");
        else if (source == "snap") args.push_back("-s");
    };

    if (command == "zinst") {
        if (row_checked(panel, "dry_run")) args.push_back("--dry-run");
        addPackages("packages");
    } else if (command == "zrm") {
        if (row_checked(panel, "purge")) args.push_back("--purge");
        if (row_checked(panel, "dry_run")) args.push_back("--dry-run");
        addPackages("packages");
    } else if (command == "zsearch") {
        addSource();
        addPackages("query");
    } else if (command == "zinfo") {
        addPackages("packages");
    } else if (command == "zlist") {
        addSource();
        if (row_checked(panel, "no_pager")) args.push_back("--no-pager");
    } else if (command == "zrun") {
        addPackages("package");
    } else if (command == "zupd") {
        if (row_checked(panel, "full")) args.push_back("--full");
        if (row_checked(panel, "yes")) args.push_back("--yes");
        if (row_checked(panel, "reboot")) args.push_back("--reboot");
        if (row_checked(panel, "shutdown")) args.push_back("--shutdown");
        if (row_checked(panel, "dry_run")) args.push_back("--dry-run");
    } else if (command == "zupgr") {
        if (row_checked(panel, "force")) args.push_back("--force");
        if (row_checked(panel, "experimental")) args.push_back("--experimental");
        if (row_checked(panel, "dry_run")) args.push_back("--dry-run");
    } else if (command == "zclean") {
        if (row_checked(panel, "dry_run")) args.push_back("--dry-run");
    } else if (command == "zhome") {
        const std::string page = row_value(panel, "page");
        if (row_checked(panel, "edit_config")) args.push_back("--edit-config");
        else if (page == "page 1") args.push_back("-p1");
        else if (page == "page 2") args.push_back("-p2");
        else if (page == "page 3") args.push_back("-p3");
    }

    return args;
}

bool zpm_command_needs_root(const std::string& command, const NativePanel& panel) {
    if (command == "zinst" || command == "zrm" || command == "zupd" ||
        command == "zupgr" || command == "zclean" || command == "zuninstall") {
        return true;
    }
    return command == "zhome" && row_checked(panel, "edit_config");
}

void execute_zpm_gui_action(NativePanel& panel) {
    if (!zpm_installed()) {
        panel.message = "ZPM is not installed in /opt/ZPM.";
        return;
    }

    const std::string command = panel.selectedKey.empty() ? "zinst" : panel.selectedKey;
    if ((command == "zinst" || command == "zrm" || command == "zinfo") &&
        trim(row_value(panel, "packages")).empty()) {
        panel.message = "Package name is required.";
        return;
    }
    if (command == "zsearch" && trim(row_value(panel, "query")).empty()) {
        panel.message = "Search query is required.";
        return;
    }
    if (command == "zrun" && trim(row_value(panel, "package")).empty()) {
        panel.message = "Program/package is required.";
        return;
    }

    const bool needsRoot = zpm_command_needs_root(command, panel);
    if (needsRoot && !ensure_root_auth(panel.message)) return;

    const std::vector<std::string> args = build_zpm_args(panel);
    std::cout << "\033[0m\033[2J\033[H";
    std::cout << CYAN << "[KSM]" << RESET << " Running ZPM function: " << command << "\n\n";
    const int code = run_command_interactive(args, needsRoot);
    std::cout << "\n" << CYAN << "[KSM]" << RESET << " Process exited with code " << code << ".\n";
    std::cout << "Press Enter to return to KSM..." << std::flush;
    std::string ignored;
    std::getline(std::cin, ignored);

    panel.message = code == 0
        ? "ZPM function completed."
        : "ZPM function failed with code " + std::to_string(code) + ".";
}

void execute_upgrade(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    std::vector<std::string> args = {"/opt/KSM/bin/kupgr"};
    if (access(args[0].c_str(), X_OK) != 0) args[0] = "kupgr";
    args.push_back("--panel");
    args.push_back("--yes");
    if (row_checked(panel, "experimental") || row_checked(panel, "repo")) args.push_back("-ex");
    if (row_checked(panel, "force")) args.push_back("-f");
    if (row_checked(panel, "repo")) args.push_back("--repo-snapshot");
    const CommandResult result = run_command_with_progress(args, true);
    const std::string logText = read_file_text("/tmp/kupgr.log");
    const std::string combinedOutput = result.output + "\n" + logText;
    const bool upToDate = update_output_up_to_date(combinedOutput);
    const bool completed = update_output_completed(combinedOutput);
    const bool success = result.code == 0 || upToDate || completed;

    if (success) {
        panel.noticeTitle = upToDate ? "ALREADY UP TO DATE" : "UPDATE COMPLETED";
        panel.noticeBody = upToDate
            ? "KSM is already on the newest version. Public command: sudo ksm."
            : "KSM update completed successfully. Public command: sudo ksm.";
        panel.noticeOk = true;
        panel.message = upToDate ? "Already up to date." : "Update completed.";
        panel.detailLines = {
            upToDate ? "KSM is already on the newest version." : "KSM update completed successfully.",
            "Public command: sudo ksm."
        };
    } else {
        panel.noticeTitle = "UPDATE FAILED";
        panel.noticeBody = "Updater exited with code " + std::to_string(result.code) + ". Check /tmp/kupgr.log.";
        panel.noticeOk = false;
        panel.message = "Upgrade failed.";
    }
}

void execute_uninstall(NativePanel& panel) {
    if (!ensure_root_auth(panel.message)) return;
    if (!row_checked(panel, "links") && !row_checked(panel, "opt")) {
        panel.message = "Select at least one uninstall action.";
        return;
    }

    int failures = 0;
    const std::vector<std::string> commands = {
        "ksm", "kcontrol", "khome", "kupgr", "kuninstall", "ksysinfo", "kserv",
        "kperm", "kssh", "kfirewall", "kgroupadd", "kgroupmod", "kgroupdel",
        "kuseradd", "kusermod", "kuserdel", "knetcfg"
    };
    if (row_checked(panel, "links")) {
        for (const auto& command : commands) {
            const fs::path link = fs::path("/usr/bin") / command;
            const fs::path expected = fs::path("/opt/KSM/bin") / command;
            std::error_code ec;
            const auto status = fs::symlink_status(link, ec);
            if (ec || !fs::exists(status) || !fs::is_symlink(status)) continue;

            const fs::path target = fs::read_symlink(link, ec);
            if (ec || target != expected) continue;

            if (run_command({"rm", "-f", link.string()}, true).code != 0) ++failures;
        }
    }
    if (row_checked(panel, "opt")) {
        if (run_command({"rm", "-rf", "/opt/KSM"}, true).code != 0) ++failures;
    }
    panel.message = failures == 0 ? "KSM uninstall actions completed." : "Uninstall failures: " + std::to_string(failures) + ".";
}

void execute_panel_action(NativePanel& panel, const std::string& id) {
    panel.armedAction.clear();
    if (panel.kind == ModuleKind::UserAdd && id == "create_users") execute_useradd(panel);
    else if (panel.kind == ModuleKind::UserDel && id == "delete_users") execute_userdel(panel);
    else if (panel.kind == ModuleKind::UserMod && id == "apply_usermod") execute_usermod(panel);
    else if (panel.kind == ModuleKind::GroupAdd && id == "create_groups") execute_groupadd(panel);
    else if (panel.kind == ModuleKind::GroupDel && id == "delete_groups") execute_groupdel(panel);
    else if (panel.kind == ModuleKind::GroupMod && id == "apply_groupmod") execute_groupmod(panel);
    else if (panel.kind == ModuleKind::Services) execute_service(panel, id);
    else if (panel.kind == ModuleKind::Permissions && id == "apply_perm") execute_perm(panel);
    else if (panel.kind == ModuleKind::Network && id == "apply_network") execute_network(panel);
    else if (panel.kind == ModuleKind::Power) execute_power(panel, id);
    else if (panel.kind == ModuleKind::SSH && id == "apply_ssh") execute_ssh(panel);
    else if (panel.kind == ModuleKind::Firewall) execute_firewall(panel, id);
    else if (panel.kind == ModuleKind::Samba) execute_samba(panel, id);
    else if (panel.kind == ModuleKind::Apache) execute_apache(panel, id);
    else if (panel.kind == ModuleKind::DnsServer) execute_dns(panel, id);
    else if (panel.kind == ModuleKind::DhcpServer) execute_dhcp(panel, id);
    else if (panel.kind == ModuleKind::FtpServer) execute_ftp(panel, id);
    else if (panel.kind == ModuleKind::ExtensionInstall && id == "install_zpm") execute_install_zpm(panel);
    else if (panel.kind == ModuleKind::ZpmExtension && id == "run_zpm_gui") execute_zpm_gui_action(panel);
    else if (panel.kind == ModuleKind::Upgrade && id == "run_upgrade") execute_upgrade(panel);
    else if (panel.kind == ModuleKind::Uninstall && id == "run_uninstall") execute_uninstall(panel);
    else panel.message = "Action not implemented.";
}

void maybe_open_picker(NativePanel& panel, PickerOverlay& picker, const Row& row) {
    if (row.id == "groups") {
        picker.active = true;
        picker.kind = PickerKind::Groups;
        picker.rowId = row.id;
        picker.title = "Choose groups";
        picker.items = picker_groups(row.value);
        picker.index = 0;
    } else if (row.id == "members") {
        picker.active = true;
        picker.kind = PickerKind::Users;
        picker.rowId = row.id;
        picker.title = "Choose users";
        picker.items = picker_users(row.value);
        picker.index = 0;
    }
}

void close_picker(NativePanel& panel, PickerOverlay& picker) {
    std::vector<std::string> selected;
    for (const auto& item : picker.items) {
        if (item.selected) selected.push_back(item.key);
    }
    set_row_value(panel, picker.rowId, join_csv(selected));
    panel.message = "Selection updated.";
    picker = {};
}

void handle_directory_select(NativePanel& panel) {
    if (panel.list.empty()) return;
    const ListItem& item = panel.list[panel.listIndex];
    std::error_code ec;
    if (item.key == "__select__") {
        set_row_value(panel, "path", fs::absolute(panel.directory, ec).lexically_normal().string());
        load_perm_current(panel);
        return;
    }
    if (item.key == "__up__" || item.directory) {
        panel.directory = item.path.empty() ? panel.directory.parent_path() : item.path;
        if (panel.directory.empty()) panel.directory = "/";
        panel.list = read_directory(panel.directory);
        panel.listIndex = 0;
        panel.message = "Directory opened.";
        return;
    }
    set_row_value(panel, "path", fs::absolute(item.path, ec).lexically_normal().string());
    load_perm_current(panel);
}

void handle_row_enter(NativePanel& panel, TextEdit& edit, PickerOverlay& picker, bool& pending, std::string& pendingAction) {
    if (panel.rows.empty()) return;
    Row& row = panel.rows[panel.rowIndex];
    if (row.type == RowType::Text) {
        if (row.id == "groups" || row.id == "members") {
            maybe_open_picker(panel, picker, row);
            return;
        }
        edit.active = true;
        edit.rowId = row.id;
        edit.title = row.label;
        edit.buffer = row.value;
        return;
    }
    if (row.type == RowType::Bool) {
        row.checked = !row.checked;
        panel.armedAction.clear();
        if (row.id == "show_system" && panel.kind == ModuleKind::UserDel) handle_local_action(panel, "refresh_users");
        if (row.id == "show_system" && panel.kind == ModuleKind::UserMod) {
            panel.list = read_users(row.checked, false);
            panel.listIndex = 0;
            panel.selectedKey.clear();
        }
        if (row.id == "show_system" && panel.kind == ModuleKind::GroupDel) handle_local_action(panel, "refresh_groups");
        if (row.id == "show_system" && panel.kind == ModuleKind::GroupMod) {
            panel.list = read_groups(row.checked, false);
            panel.listIndex = 0;
            panel.selectedKey.clear();
        }
        return;
    }
    if (row.type == RowType::Choice) {
        cycle_choice(row, 1);
        if (panel.kind == ModuleKind::Home && row.id == "page") handle_local_action(panel, "refresh_home");
        return;
    }
    if (row.type == RowType::Action) {
        if (is_local_action(row.id)) {
            handle_local_action(panel, row.id);
            return;
        }
        if (row.danger && panel.armedAction != row.id) {
            panel.armedAction = row.id;
            panel.message = "Press Enter again to confirm: " + row.label + ".";
            return;
        }
        pending = true;
        pendingAction = row.id;
    }
}

int run_tui() {
    auto items = categories();
    int categoryIndex = 0;
    int moduleIndex = 0;
    std::string message;
    bool showHelp = false;
    bool shouldQuit = false;
    bool pending = false;
    std::string pendingAction;
    FocusPane menuFocus = FocusPane::Categories;
    ViewMode view = ViewMode::Menu;
    NativePanel panel;
    TextEdit edit;
    PickerOverlay picker;

    while (!shouldQuit) {
        pending = false;
        pendingAction.clear();
        {
            auto screen = ScreenInteractive::Fullscreen();
            auto exitLoop = screen.ExitLoopClosure();

            auto root = Renderer([&] {
                if (view == ViewMode::NativePanel) {
                    return render_native_panel(panel, showHelp, edit, picker);
                }
                return render_menu(items, categoryIndex, moduleIndex, message, showHelp, menuFocus);
            });

            root = CatchEvent(root, [&](Event event) {
            if (!panel.noticeTitle.empty()) {
                if (event == Event::Return || event == Event::Escape ||
                    event == Event::Character("q") || event == Event::Character("Q")) {
                    panel.noticeTitle.clear();
                    panel.noticeBody.clear();
                    panel.noticeOk = false;
                    return true;
                }
                return true;
            }

            if (edit.active) {
                if (event == Event::Escape) {
                    edit = {};
                    return true;
                }
                if (event == Event::Return) {
                    set_row_value(panel, edit.rowId, edit.buffer);
                    panel.message = "Field updated.";
                    edit = {};
                    return true;
                }
                if (event == Event::Backspace) {
                    if (!edit.buffer.empty()) edit.buffer.pop_back();
                    return true;
                }
                if (event.is_character()) {
                    edit.buffer += event.character()[0];
                    return true;
                }
                return true;
            }

            if (picker.active) {
                if (event == Event::Escape || event == Event::Character("q") || event == Event::Character("Q")) {
                    close_picker(panel, picker);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    --picker.index;
                    if (picker.index < 0) picker.index = static_cast<int>(picker.items.size()) - 1;
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Tab) {
                    ++picker.index;
                    if (picker.index >= static_cast<int>(picker.items.size())) picker.index = 0;
                    return true;
                }
                if (event == Event::Return && !picker.items.empty()) {
                    picker.items[picker.index].selected = !picker.items[picker.index].selected;
                    return true;
                }
                return true;
            }

            if (event == Event::F1 || event == Event::Character("?")) {
                showHelp = !showHelp;
                return true;
            }
            if (showHelp) {
                showHelp = false;
                return true;
            }

            if (view == ViewMode::Menu) {
                if (event == Event::Character("q") || event == Event::Character("Q") || event == Event::Escape) {
                    shouldQuit = true;
                    exitLoop();
                    return true;
                }
                if (event == Event::ArrowLeft || event == Event::ArrowRight || event == Event::Tab) {
                    menuFocus = menuFocus == FocusPane::Categories ? FocusPane::Modules : FocusPane::Categories;
                    return true;
                }
                if (event == Event::ArrowUp) {
                    if (menuFocus == FocusPane::Categories) {
                        --categoryIndex;
                        if (categoryIndex < 0) categoryIndex = static_cast<int>(items.size()) - 1;
                        clamp_module(items, categoryIndex, moduleIndex);
                    } else {
                        --moduleIndex;
                        if (moduleIndex < 0) moduleIndex = static_cast<int>(items[categoryIndex].modules.size()) - 1;
                    }
                    return true;
                }
                if (event == Event::ArrowDown) {
                    if (menuFocus == FocusPane::Categories) {
                        ++categoryIndex;
                        if (categoryIndex >= static_cast<int>(items.size())) categoryIndex = 0;
                        clamp_module(items, categoryIndex, moduleIndex);
                    } else {
                        ++moduleIndex;
                        if (moduleIndex >= static_cast<int>(items[categoryIndex].modules.size())) moduleIndex = 0;
                    }
                    return true;
                }
                if (event == Event::Return) {
                    if (menuFocus == FocusPane::Categories) {
                        menuFocus = FocusPane::Modules;
                    } else {
                        panel = make_native_panel(items[categoryIndex].modules[moduleIndex]);
                        view = ViewMode::NativePanel;
                        showHelp = false;
                    }
                    return true;
                }
                return false;
            }

            if (event == Event::Character("q") || event == Event::Character("Q") || event == Event::Escape) {
                view = ViewMode::Menu;
                showHelp = false;
                panel = {};
                return true;
            }
            if (event == Event::ArrowLeft) {
                if (panel.kind == ModuleKind::Permissions && panel.focus == PanelFocus::List) {
                    panel.directory = panel.directory.parent_path();
                    if (panel.directory.empty()) panel.directory = "/";
                    panel.list = read_directory(panel.directory);
                    panel.listIndex = 0;
                    return true;
                }
                if (!panel.list.empty()) {
                    panel.focus = PanelFocus::List;
                    return true;
                }
                if (!panel.rows.empty() && panel.rows[panel.rowIndex].type == RowType::Choice) {
                    cycle_choice(panel.rows[panel.rowIndex], -1);
                    return true;
                }
            }
            if (event == Event::ArrowRight) {
                if (!panel.list.empty()) {
                    panel.focus = PanelFocus::Rows;
                    return true;
                }
                if (!panel.rows.empty() && panel.rows[panel.rowIndex].type == RowType::Choice) {
                    cycle_choice(panel.rows[panel.rowIndex], 1);
                    return true;
                }
            }
            if (event == Event::Tab) {
                if (!panel.list.empty()) {
                    panel.focus = panel.focus == PanelFocus::List ? PanelFocus::Rows : PanelFocus::List;
                } else if (!panel.rows.empty()) {
                    panel.rowIndex = static_cast<int>(panel.rows.size()) - 1;
                }
                return true;
            }
            if (event == Event::ArrowUp) {
                if (panel.focus == PanelFocus::List && !panel.list.empty()) {
                    --panel.listIndex;
                    if (panel.listIndex < 0) panel.listIndex = static_cast<int>(panel.list.size()) - 1;
                } else if (!panel.rows.empty()) {
                    --panel.rowIndex;
                    if (panel.rowIndex < 0) panel.rowIndex = static_cast<int>(panel.rows.size()) - 1;
                }
                return true;
            }
            if (event == Event::ArrowDown) {
                if (panel.focus == PanelFocus::List && !panel.list.empty()) {
                    ++panel.listIndex;
                    if (panel.listIndex >= static_cast<int>(panel.list.size())) panel.listIndex = 0;
                } else if (!panel.rows.empty()) {
                    ++panel.rowIndex;
                    if (panel.rowIndex >= static_cast<int>(panel.rows.size())) panel.rowIndex = 0;
                }
                return true;
            }
            if (event == Event::Return) {
                if (panel.focus == PanelFocus::List && !panel.list.empty()) {
                    if (panel.kind == ModuleKind::Permissions) {
                        handle_directory_select(panel);
                    } else if (panel.kind == ModuleKind::ZpmExtension) {
                        const ListItem& item = panel.list[panel.listIndex];
                        load_zpm_function_panel(panel, item.key);
                    } else {
                        toggle_current_list_item(panel);
                    }
                    return true;
                }
                handle_row_enter(panel, edit, picker, pending, pendingAction);
                if (pending) exitLoop();
                return true;
            }
            return false;
            });

            screen.Loop(root);
        }
        if (pending) {
            execute_panel_action(panel, pendingAction);
            items = categories();
            if (categoryIndex >= static_cast<int>(items.size())) categoryIndex = static_cast<int>(items.size()) - 1;
            if (categoryIndex < 0) categoryIndex = 0;
            clamp_module(items, categoryIndex, moduleIndex);
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
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
        std::cerr << "run 'ksm --help' to list options.\n";
        return 1;
    }
    return run_tui();
}
