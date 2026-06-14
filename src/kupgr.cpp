#include "main.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace {

namespace fs = std::filesystem;

constexpr const char* kInstallDir = "/opt/KSM";
constexpr const char* kBinDir = "/usr/bin";
constexpr const char* kLogPath = "/tmp/kupgr.log";
constexpr const char* kReleasesApiUrl =
    "https://api.github.com/repos/Zielina-Konrad-productions/KSM/releases";
constexpr const char* kRepoArchiveUrl =
    "https://github.com/Zielina-Konrad-productions/KSM/archive/refs/heads/main.tar.gz";

enum class Key {
    Up,
    Down,
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

struct ReleaseInfo {
    std::string tag;
    std::string version;
    std::string archiveUrl;
    bool experimental = false;

    bool valid() const {
        return !tag.empty() && !version.empty() && !archiveUrl.empty();
    }
};

struct Options {
    bool installMissingTools = false;
    bool preserveConfig = true;
    bool experimental = false;
    bool repoSnapshot = false;
    bool force = false;
    bool buildProject = true;
    bool linkCommands = true;
    std::string localVersion;
    std::string remoteVersion;
    ReleaseInfo remoteRelease;
    std::string message;
};

struct ProcessResult {
    int exitCode = 1;
    std::string output;
};

struct ProcessConfig {
    std::string cwd;
    bool captureStdout = false;
    bool quietStderr = false;
    std::vector<std::pair<std::string, std::string>> env;
};

enum class UpdateResult {
    Updated,
    UpToDate,
    Failed
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

class TempDirectory {
public:
    static std::optional<TempDirectory> create() {
        std::string pattern = "/tmp/ksm-upgrade-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');

        char* path = mkdtemp(buffer.data());
        if (path == nullptr) {
            return std::nullopt;
        }
        return TempDirectory(fs::path(path));
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    TempDirectory(TempDirectory&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    ~TempDirectory() {
        cleanup();
    }

    const fs::path& path() const {
        return path_;
    }

private:
    explicit TempDirectory(fs::path path) : path_(std::move(path)) {}

    void cleanup() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
        path_.clear();
    }

    fs::path path_;
};

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

void banner() {
    std::cout << BLUE;
    std::cout << "========================================\n";
    std::cout << "      Kastiusz System Manager\n";
    std::cout << "              kupgr\n";
    std::cout << "========================================\n";
    std::cout << RESET;
}

void version() {
    std::cout << BLUE << "kupgr component version: v" << ksm_version::version() << RESET << '\n';
    std::cout << "Kastiusz System Manager\n";
    std::cout << "License: MIT\n";
}

void help() {
    std::cout << BLUE << "Usage: " << RESET << "sudo kupgr [options]\n";
    std::cout << "Interactive terminal GUI for updating KSM from GitHub Releases.\n\n";
    std::cout << BLUE << "Repository:" << RESET << '\n';
    std::cout << "  Zielina-Konrad-productions/KSM releases\n\n";
    std::cout << BLUE << "Options:" << RESET << '\n';
    std::cout << "  --experimental, -ex  Use latest prerelease\n";
    std::cout << "  --force, -f          Start with force reinstall enabled\n";
    std::cout << "  --help, -h           Show this help\n";
    std::cout << "  --version, -v        Show version information\n\n";
    std::cout << BLUE << "Controls:" << RESET << '\n';
    std::cout << "  Up/Down         Move\n";
    std::cout << "  Tab             Jump to Start/Top\n";
    std::cout << "  Enter           Toggle option, refresh, or run action\n";
    std::cout << "  q               Cancel\n";
    std::cout << '\n' << BLUE << "Experimental:" << RESET << '\n';
    std::cout << "  Repo snapshot appears in experimental mode and installs main as VERSION.txt + -snap.\n";
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string normalize_version(std::string value) {
    value = trim(value);
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    return value;
}

std::string lower_text(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string snapshot_version(std::string value) {
    value = normalize_version(value);
    if (value.empty()) {
        return "";
    }
    const std::string lower = lower_text(value);
    if (lower.size() >= 5 && lower.substr(lower.size() - 5) == "-snap") {
        return value;
    }
    return value + "-snap";
}

bool is_prerelease_version(const std::string& version) {
    const std::string lower = lower_text(version);
    return lower.find("pre") != std::string::npos ||
           lower.find("alpha") != std::string::npos ||
           lower.find("beta") != std::string::npos ||
           lower.find("rc") != std::string::npos;
}

bool installing_stable_over_prerelease(const Options& options) {
    return !options.experimental && is_prerelease_version(options.localVersion);
}

std::string json_unescape(const std::string& value) {
    std::string out;
    bool escape = false;

    for (char ch : value) {
        if (escape) {
            if (ch == '"' || ch == '\\' || ch == '/') {
                out.push_back(ch);
            } else if (ch == 'n') {
                out.push_back('\n');
            } else if (ch == 'r') {
                out.push_back('\r');
            } else if (ch == 't') {
                out.push_back('\t');
            } else {
                out.push_back(ch);
            }
            escape = false;
            continue;
        }

        if (ch == '\\') {
            escape = true;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::optional<std::string> json_string_value(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = object.find(needle);
    if (pos == std::string::npos) return std::nullopt;

    pos = object.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;

    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    if (pos >= object.size() || object[pos] != '"') return std::nullopt;
    ++pos;

    std::string raw;
    bool escape = false;
    while (pos < object.size()) {
        const char ch = object[pos++];
        if (escape) {
            raw.push_back('\\');
            raw.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return json_unescape(raw);
        }
        raw.push_back(ch);
    }
    return std::nullopt;
}

std::optional<bool> json_bool_value(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = object.find(needle);
    if (pos == std::string::npos) return std::nullopt;

    pos = object.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;

    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    if (object.compare(pos, 4, "true") == 0) return true;
    if (object.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

std::vector<std::string> release_objects(const std::string& json) {
    std::vector<std::string> objects;
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        const char ch = json[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
            continue;
        }
        if (ch == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

std::string url_encode_tag(const std::string& value) {
    std::ostringstream out;
    const char* hex = "0123456789ABCDEF";

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << hex[ch >> 4] << hex[ch & 15];
        }
    }
    return out.str();
}

std::string release_version_from_tag(const std::string& tag) {
    std::string value = normalize_version(tag);
    const size_t slash = value.find_last_of('/');
    if (slash != std::string::npos) value = value.substr(slash + 1);
    if (value.rfind("KSM-", 0) == 0 || value.rfind("ksm-", 0) == 0) {
        value = value.substr(4);
    }
    const auto firstDigit = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch);
    });
    if (firstDigit != value.end()) {
        value.erase(value.begin(), firstDigit);
    }
    return normalize_version(value);
}

std::optional<ReleaseInfo> parse_release(const std::string& object, bool experimental) {
    const bool prerelease = json_bool_value(object, "prerelease").value_or(false);
    const bool draft = json_bool_value(object, "draft").value_or(false);
    if (draft || prerelease != experimental) return std::nullopt;

    const auto tag = json_string_value(object, "tag_name");
    if (!tag || trim(*tag).empty()) return std::nullopt;

    ReleaseInfo release;
    release.tag = trim(*tag);
    release.version = release_version_from_tag(release.tag);
    release.experimental = experimental;

    const auto tarball = json_string_value(object, "tarball_url");
    if (tarball && !trim(*tarball).empty()) {
        release.archiveUrl = trim(*tarball);
    } else {
        release.archiveUrl = "https://github.com/Zielina-Konrad-productions/KSM/archive/refs/tags/" +
                             url_encode_tag(release.tag) + ".tar.gz";
    }

    if (!release.valid()) return std::nullopt;
    return release;
}

std::optional<ReleaseInfo> parse_github_release(const std::string& json, bool experimental) {
    for (const auto& object : release_objects(json)) {
        auto release = parse_release(object, experimental);
        if (release) return release;
    }
    return std::nullopt;
}

std::vector<int> version_numbers(const std::string& value) {
    std::vector<int> numbers;
    std::string current;

    for (unsigned char ch : value) {
        if (std::isdigit(ch)) {
            current.push_back(static_cast<char>(ch));
        } else if (!current.empty()) {
            numbers.push_back(std::stoi(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        numbers.push_back(std::stoi(current));
    }
    return numbers;
}

int compare_versions(const std::string& left, const std::string& right) {
    const auto a = version_numbers(left);
    const auto b = version_numbers(right);
    const size_t size = std::max(a.size(), b.size());

    for (size_t i = 0; i < size; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

void log_line(const std::string& line, bool truncate = false) {
    std::ofstream log(kLogPath, truncate ? std::ios::trunc : std::ios::app);
    if (log.is_open()) {
        log << line << '\n';
    }
}

void redirect_to_dev_null(int fd) {
    const int flags = (fd == STDIN_FILENO) ? O_RDONLY : O_WRONLY;
    const int nullFd = open("/dev/null", flags | O_CLOEXEC);
    if (nullFd >= 0) {
        dup2(nullFd, fd);
        close(nullFd);
    }
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
        if (!config.cwd.empty() && chdir(config.cwd.c_str()) != 0) {
            _exit(126);
        }
        for (const auto& item : config.env) {
            setenv(item.first.c_str(), item.second.c_str(), 1);
        }

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
    if (waitpid(pid, &status, 0) < 0) {
        return result;
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
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

std::string downloader() {
    if (command_exists("curl")) return "curl";
    if (command_exists("wget")) return "wget";
    return "";
}

std::string read_first_line(const fs::path& path) {
    std::ifstream file(path);
    std::string line;
    if (std::getline(file, line)) {
        return trim(line);
    }
    return "";
}

std::string read_local_version() {
    return normalize_version(read_first_line(fs::path(kInstallDir) / "VERSION.txt"));
}

std::optional<ReleaseInfo> fetch_remote_release(bool experimental) {
    const std::string tool = downloader();
    if (tool.empty()) return std::nullopt;

    ProcessConfig config;
    config.captureStdout = true;
    config.quietStderr = true;

    ProcessResult result;
    if (tool == "curl") {
        result = run_process({"curl", "-fsSL", kReleasesApiUrl}, config);
    } else {
        result = run_process({"wget", "-qO-", kReleasesApiUrl}, config);
    }

    if (result.exitCode != 0 || result.output.empty()) return std::nullopt;
    return parse_github_release(result.output, experimental);
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
        return {Key::Unknown, '\0'};
    }
    if (c >= 32 && c <= 126) return {Key::Character, c};
    return {Key::Unknown, '\0'};
}

std::string yes_no(bool value) {
    return value ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
}

std::string version_label(const std::string& value) {
    return value.empty() ? DIM + "(unknown)" + RESET : "v" + value;
}

std::string channel_label(bool experimental) {
    return experimental ? "experimental prerelease" : "release";
}

std::string source_label(const Options& options) {
    return options.repoSnapshot ? "GitHub repo main branch" : "GitHub Releases";
}

std::string release_tag_label(const ReleaseInfo& release) {
    return release.tag.empty() ? DIM + "(none)" + RESET : release.tag;
}

std::string newest_version_message(const std::string& version) {
    std::string message = "Already on newest version";
    if (!version.empty()) {
        message += " (v" + version + ")";
    }
    message += ". Nothing to do.";
    return message;
}

bool is_newest_version_message(const std::string& message) {
    return message.rfind("Already on newest version", 0) == 0;
}

int tools_row(const Options& options) {
    return options.experimental ? 3 : 2;
}

int start_row(const Options& options) {
    return options.experimental ? 8 : 7;
}

int cancel_row(const Options& options) {
    return options.experimental ? 9 : 8;
}

int max_row(const Options& options) {
    return cancel_row(options);
}

void draw_row(int row, int cursor, const std::string& text, bool action = false, bool danger = false) {
    const bool active = row == cursor;
    std::cout << (active ? BLUE : "");
    std::cout << (active ? "> " : "  ");
    if (danger) {
        std::cout << RED;
    } else if (action) {
        std::cout << GREEN;
    }
    std::cout << text << RESET << '\n';
}

void draw(const Options& options, int cursor) {
    clear_screen();
    banner();
    std::cout << CYAN << "Arrows:" << RESET << " move  "
              << CYAN << "Tab:" << RESET << " jump  "
              << CYAN << "Enter:" << RESET << " toggle/action  "
              << CYAN << "q:" << RESET << " cancel\n\n";

    std::cout << BOLD << "Update source" << RESET << '\n';
    std::cout << "  Repo  : Zielina-Konrad-productions/KSM\n";
    std::cout << "  Source: " << source_label(options) << '\n';
    std::cout << "  Channel: " << channel_label(options.experimental) << '\n';
    std::cout << "  Tag   : " << release_tag_label(options.remoteRelease) << '\n';
    std::cout << "  Local : " << version_label(options.localVersion) << '\n';
    std::cout << "  Remote: " << version_label(options.remoteVersion) << "\n\n";

    draw_row(0, cursor, "Refresh version info  Enter");
    draw_row(1, cursor, "Experimental release  " + yes_no(options.experimental));
    const int offset = options.experimental ? 1 : 0;
    if (options.experimental) {
        draw_row(2, cursor, "Repo snapshot         " + yes_no(options.repoSnapshot));
    }
    draw_row(2 + offset, cursor, "Install missing tools " + yes_no(options.installMissingTools));
    draw_row(3 + offset, cursor, "Preserve config       " + yes_no(options.preserveConfig));
    draw_row(4 + offset, cursor, "Force reinstall       " + yes_no(options.force));
    draw_row(5 + offset, cursor, "Build C++ programs    " + yes_no(options.buildProject));
    draw_row(6 + offset, cursor, "Link commands         " + yes_no(options.linkCommands));
    draw_row(start_row(options), cursor, "Start update          Enter", true);
    draw_row(cancel_row(options), cursor, "Cancel                Enter or q");

    if (!options.message.empty()) {
        std::cout << '\n' << YELLOW << options.message << RESET << '\n';
    }
    std::cout << std::flush;
}

void move_cursor(int& cursor, int delta, int maxRow) {
    cursor += delta;
    if (cursor < 0) cursor = maxRow;
    if (cursor > maxRow) cursor = 0;
}

std::string detect_package_manager() {
    if (command_exists("apt-get")) return "apt";
    if (command_exists("zypper")) return "zypper";
    if (command_exists("dnf")) return "dnf";
    return "unknown";
}

std::vector<std::string> update_dependencies_for(const std::string& pm) {
    if (pm == "apt") return {"curl", "tar", "gzip", "g++", "bash", "coreutils", "systemd"};
    if (pm == "zypper") return {"curl", "tar", "gzip", "gcc-c++", "bash", "coreutils", "systemd"};
    if (pm == "dnf") return {"curl", "tar", "gzip", "gcc-c++", "bash", "coreutils", "systemd"};
    return {};
}

bool install_missing_tools() {
    const std::string pm = detect_package_manager();
    const auto deps = update_dependencies_for(pm);
    if (deps.empty()) {
        std::cout << YELLOW << "[!]" << RESET << " Unsupported package manager. Install curl, tar, gzip, g++, bash and resolvectl manually.\n";
        return false;
    }

    std::cout << CYAN << "[*]" << RESET << " Installing updater tools with " << pm << "...\n";
    if (pm == "apt") {
        if (run_process({"apt-get", "update", "-y"}).exitCode != 0) return false;
        std::vector<std::string> args = {"apt-get", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        return run_process(args).exitCode == 0;
    }
    if (pm == "zypper") {
        if (run_process({"zypper", "--non-interactive", "refresh"}).exitCode != 0) return false;
        std::vector<std::string> args = {"zypper", "--non-interactive", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        return run_process(args).exitCode == 0;
    }
    if (pm == "dnf") {
        std::vector<std::string> args = {"dnf", "install", "-y"};
        args.insert(args.end(), deps.begin(), deps.end());
        return run_process(args).exitCode == 0;
    }
    return false;
}

bool required_tools_available(const Options& options, std::string& missing) {
    std::vector<std::string> missingTools;
    if (downloader().empty()) missingTools.push_back("curl or wget");
    if (!command_exists("tar")) missingTools.push_back("tar");
    if (!command_exists("gzip")) missingTools.push_back("gzip");
    if (!command_exists("bash")) missingTools.push_back("bash");
    if (options.buildProject && !command_exists("g++")) missingTools.push_back("g++");
    if (options.linkCommands && !command_exists("ln")) missingTools.push_back("ln");

    if (missingTools.empty()) return true;
    for (const auto& tool : missingTools) {
        if (!missing.empty()) missing += ", ";
        missing += tool;
    }
    return false;
}

bool valid_source_tree(const fs::path& source) {
    return fs::is_regular_file(source / "VERSION.txt") &&
           fs::is_regular_file(source / "kastiusz.conf") &&
           fs::is_directory(source / "src") &&
           fs::is_regular_file(source / "src" / "build.sh") &&
           fs::is_regular_file(source / "src" / "KSM.cpp");
}

bool confirm_update(const Options& options) {
    clear_screen();
    banner();
    std::cout << YELLOW << "Ready to update KSM" << RESET << "\n\n";
    std::cout << "Source              : " << source_label(options) << '\n';
    std::cout << "Channel             : " << channel_label(options.experimental) << '\n';
    std::cout << "Release tag         : " << release_tag_label(options.remoteRelease) << '\n';
    std::cout << "Local version       : " << version_label(options.localVersion) << '\n';
    std::cout << "Remote version      : " << version_label(options.remoteVersion) << '\n';
    std::cout << "Preserve config     : " << (options.preserveConfig ? "yes" : "no") << '\n';
    std::cout << "Force reinstall     : " << (options.force ? "yes" : "no") << '\n';
    std::cout << "Build project       : " << (options.buildProject ? "yes" : "no") << '\n';
    std::cout << "Link commands       : " << (options.linkCommands ? "yes" : "no") << "\n\n";
    if (installing_stable_over_prerelease(options)) {
        std::cout << YELLOW << "Warning:" << RESET
                  << " installed version looks experimental/pre; stable release will be installed.\n\n";
    }
    std::cout << "Press " << BLUE << "Enter" << RESET << " or " << BLUE << "y" << RESET
              << " to update, any other key to return.\n";

    const KeyPress key = read_key();
    return key.key == Key::Enter ||
           (key.key == Key::Character && (key.value == 'y' || key.value == 'Y'));
}

bool download_archive(const ReleaseInfo& release, const fs::path& archivePath) {
    const std::string tool = downloader();
    if (tool == "curl") {
        return run_process({"curl", "-fL", "--retry", "2", "-o", archivePath.string(), release.archiveUrl}).exitCode == 0;
    }
    if (tool == "wget") {
        return run_process({"wget", "-O", archivePath.string(), release.archiveUrl}).exitCode == 0;
    }
    return false;
}

bool download_repo_snapshot(const fs::path& archivePath) {
    const std::string tool = downloader();
    if (tool == "curl") {
        return run_process({"curl", "-fL", "--retry", "2", "-o", archivePath.string(), kRepoArchiveUrl}).exitCode == 0;
    }
    if (tool == "wget") {
        return run_process({"wget", "-O", archivePath.string(), kRepoArchiveUrl}).exitCode == 0;
    }
    return false;
}

std::optional<fs::path> extract_archive(const fs::path& archivePath, const fs::path& extractDir) {
    std::error_code ec;
    fs::create_directories(extractDir, ec);
    if (ec) return std::nullopt;

    if (run_process({"tar", "-xzf", archivePath.string(), "-C", extractDir.string()}).exitCode != 0) {
        return std::nullopt;
    }

    for (const auto& entry : fs::directory_iterator(extractDir, ec)) {
        if (!ec && entry.is_directory()) {
            return entry.path();
        }
    }
    return std::nullopt;
}

bool preserve_config_file(const fs::path& sourceDir) {
    const fs::path installedConfig = fs::path(kInstallDir) / "kastiusz.conf";
    if (!fs::is_regular_file(installedConfig)) {
        log_line("config: no installed config to preserve");
        return true;
    }

    std::error_code ec;
    fs::copy_file(installedConfig, sourceDir / "kastiusz.conf", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_line("config: cannot preserve kastiusz.conf: " + ec.message());
        return false;
    }
    log_line("config: preserved installed kastiusz.conf");
    return true;
}

bool write_version_file(const fs::path& directory, const std::string& version) {
    std::ofstream file(directory / "VERSION.txt", std::ios::trunc);
    if (!file.is_open()) {
        log_line("version: cannot write " + (directory / "VERSION.txt").string());
        return false;
    }

    file << version << '\n';
    if (!file.good()) {
        log_line("version: failed while writing " + (directory / "VERSION.txt").string());
        return false;
    }

    log_line("version: wrote " + (directory / "VERSION.txt").string() + " as " + version);
    return true;
}

bool write_release_version_file(const fs::path& directory, const ReleaseInfo& release) {
    if (release.version.empty()) {
        log_line("version: release version is empty");
        return false;
    }
    if (!write_version_file(directory, release.version)) {
        return false;
    }

    const std::string reread = normalize_version(read_first_line(directory / "VERSION.txt"));
    if (reread != release.version) {
        log_line("version: verification failed, got " + reread + " expected " + release.version);
        return false;
    }
    return true;
}

bool build_project(const fs::path& sourceDir) {
    ProcessConfig config;
    config.cwd = (sourceDir / "src").string();
    config.env.push_back({"KSM_TARGET", sourceDir.string()});
    config.env.push_back({"KSM_SRC_DIR", (sourceDir / "src").string()});
    return run_process({"bash", "build.sh"}, config).exitCode == 0;
}

fs::path unique_opt_path(const std::string& prefix) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return fs::path("/opt") / (prefix + "." + std::to_string(getpid()) + "." + std::to_string(seconds));
}

bool copy_tree_to_opt(const fs::path& sourceDir, const fs::path& targetDir) {
    std::error_code ec;
    fs::copy(sourceDir, targetDir, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        log_line("install: cannot prepare new tree: " + ec.message());
        return false;
    }
    return true;
}

bool link_commands() {
    const fs::path binDir = fs::path(kInstallDir) / "bin";
    if (!fs::is_directory(binDir)) {
        log_line("links: missing bin directory");
        return false;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(binDir, ec)) {
        if (ec) {
            log_line("links: cannot read bin directory: " + ec.message());
            return false;
        }
        if (!entry.is_regular_file()) continue;
        const fs::path link = fs::path(kBinDir) / entry.path().filename();
        if (run_process({"ln", "-sf", entry.path().string(), link.string()}).exitCode != 0) {
            log_line("links: failed to link " + link.string());
            return false;
        }
    }
    return true;
}

bool rollback_install(const fs::path& backupDir) {
    if (backupDir.empty() || !fs::exists(backupDir)) return false;

    std::error_code ec;
    fs::remove_all(kInstallDir, ec);
    if (ec) {
        log_line("rollback: cannot remove failed install: " + ec.message());
        return false;
    }

    fs::rename(backupDir, kInstallDir, ec);
    if (ec) {
        log_line("rollback: cannot restore backup: " + ec.message());
        return false;
    }
    log_line("rollback: previous installation restored");
    return true;
}

bool commit_install(const fs::path& newInstallDir, bool doLinkCommands, const ReleaseInfo& release) {
    const fs::path backupDir = unique_opt_path("KSM.backup.kupgr");
    bool hasBackup = false;
    std::error_code ec;

    if (fs::exists(kInstallDir, ec)) {
        fs::rename(kInstallDir, backupDir, ec);
        if (ec) {
            log_line("install: cannot move current installation to backup: " + ec.message());
            return false;
        }
        hasBackup = true;
    }

    fs::rename(newInstallDir, kInstallDir, ec);
    if (ec) {
        log_line("install: cannot activate new installation: " + ec.message());
        if (hasBackup) rollback_install(backupDir);
        return false;
    }

    if (!write_release_version_file(kInstallDir, release)) {
        if (hasBackup) rollback_install(backupDir);
        return false;
    }

    if (doLinkCommands && !link_commands()) {
        if (hasBackup) rollback_install(backupDir);
        return false;
    }

    if (hasBackup) {
        fs::remove_all(backupDir, ec);
        if (ec) {
            log_line("install: backup cleanup failed: " + ec.message());
        }
    }
    return true;
}

UpdateResult run_update(const Options& options) {
    log_line("---starting KSM update---", true);
    log_line("channel: " + channel_label(options.experimental));
    log_line("source: " + source_label(options));
    log_line("local: " + (options.localVersion.empty() ? "unknown" : options.localVersion));

    auto temp = TempDirectory::create();
    if (!temp) {
        std::cout << RED << "[x]" << RESET << " Cannot create temporary directory.\n";
        log_line("temp: cannot create temporary directory");
        return UpdateResult::Failed;
    }

    ReleaseInfo release = options.remoteRelease;
    const fs::path archivePath = temp->path() / "KSM-release.tar.gz";
    const fs::path extractDir = temp->path() / "extract";
    const fs::path newInstallDir = unique_opt_path("KSM.new.kupgr");

    std::cout << CYAN << "[*]" << RESET << " Checking required tools...\n";
    std::string missing;
    if (!required_tools_available(options, missing)) {
        log_line("tools: missing " + missing);
        if (!options.installMissingTools || !install_missing_tools()) {
            std::cout << RED << "[x]" << RESET << " Missing tools: " << missing << '\n';
            return UpdateResult::Failed;
        }
    }

    if (options.repoSnapshot) {
        release.tag = "main";
        release.archiveUrl = kRepoArchiveUrl;
        release.experimental = true;
    } else if (!release.valid()) {
        std::cout << CYAN << "[*]" << RESET << " Fetching GitHub release metadata...\n";
        const auto fetched = fetch_remote_release(options.experimental);
        if (!fetched) {
            std::cout << RED << "[x]" << RESET << " No "
                      << channel_label(options.experimental) << " found on GitHub Releases.\n";
            log_line("release: no matching release found");
            return UpdateResult::Failed;
        }
        release = *fetched;
    }

    log_line("release tag: " + release.tag);
    if (!release.version.empty()) log_line("remote: " + release.version);

    const bool stableOverPrerelease = !options.experimental && is_prerelease_version(options.localVersion);
    if (stableOverPrerelease) {
        std::cout << YELLOW << "[!]" << RESET
                  << " Installed version looks experimental/pre. Installing latest stable release.\n";
        log_line("warning: stable install over prerelease " + options.localVersion);
    }

    if (!options.force && !options.localVersion.empty() &&
        !options.repoSnapshot &&
        !stableOverPrerelease &&
        compare_versions(options.localVersion, release.version) >= 0) {
        std::cout << GREEN << "[+]" << RESET << " " << newest_version_message(release.version) << '\n';
        log_line("update: already up to date");
        return UpdateResult::UpToDate;
    }

    std::cout << CYAN << "[*]" << RESET << " Downloading KSM "
              << (options.repoSnapshot ? "repo snapshot" : "release archive") << "...\n";
    const bool downloaded = options.repoSnapshot ? download_repo_snapshot(archivePath) : download_archive(release, archivePath);
    if (!downloaded) {
        log_line("download: failed");
        return UpdateResult::Failed;
    }

    std::cout << CYAN << "[*]" << RESET << " Extracting source archive...\n";
    const auto sourceDir = extract_archive(archivePath, extractDir);
    if (!sourceDir || !valid_source_tree(*sourceDir)) {
        log_line("extract: source tree is invalid");
        return UpdateResult::Failed;
    }

    if (options.repoSnapshot) {
        release.version = snapshot_version(read_first_line(*sourceDir / "VERSION.txt"));
        if (release.version.empty()) {
            log_line("version: repo snapshot VERSION.txt is empty");
            return UpdateResult::Failed;
        }
        log_line("remote: " + release.version);
    }

    if (options.preserveConfig) {
        std::cout << CYAN << "[*]" << RESET << " Preserving kastiusz.conf...\n";
        if (!preserve_config_file(*sourceDir)) {
            return UpdateResult::Failed;
        }
    }

    std::cout << CYAN << "[*]" << RESET << " Writing VERSION.txt as v" << release.version << "...\n";
    if (!write_release_version_file(*sourceDir, release)) {
        return UpdateResult::Failed;
    }

    if (options.buildProject) {
        std::cout << CYAN << "[*]" << RESET << " Building C++ programs...\n";
        if (!build_project(*sourceDir)) {
            log_line("build: failed");
            return UpdateResult::Failed;
        }
    } else {
        std::cout << YELLOW << "[!]" << RESET << " Build skipped.\n";
        log_line("build: skipped");
    }

    std::cout << CYAN << "[*]" << RESET << " Preparing new installation tree...\n";
    if (!copy_tree_to_opt(*sourceDir, newInstallDir)) {
        std::error_code ec;
        fs::remove_all(newInstallDir, ec);
        return UpdateResult::Failed;
    }

    std::cout << CYAN << "[*]" << RESET << " Activating new KSM installation...\n";
    if (!commit_install(newInstallDir, options.linkCommands, release)) {
        std::error_code ec;
        fs::remove_all(newInstallDir, ec);
        return UpdateResult::Failed;
    }

    log_line("update: complete");
    return UpdateResult::Updated;
}

bool validate_before_update(Options& options) {
    options.localVersion = read_local_version();
    if (!options.experimental) {
        options.repoSnapshot = false;
    }

    if (options.repoSnapshot) {
        options.remoteRelease = {};
        options.remoteRelease.tag = "main";
        options.remoteRelease.archiveUrl = kRepoArchiveUrl;
        options.remoteRelease.experimental = true;
        options.remoteVersion.clear();
        options.message.clear();
        return true;
    }

    if (!options.remoteRelease.valid()) {
        if (downloader().empty() && options.installMissingTools) {
            options.message.clear();
            return true;
        }

        const auto fetched = fetch_remote_release(options.experimental);
        if (fetched) {
            options.remoteRelease = *fetched;
            options.remoteVersion = fetched->version;
        }
    }

    if (!options.remoteRelease.valid()) {
        options.message = "No " + channel_label(options.experimental) + " found on GitHub Releases.";
        return false;
    }

    if (!options.force && !options.localVersion.empty() &&
        !installing_stable_over_prerelease(options) &&
        compare_versions(options.localVersion, options.remoteVersion) >= 0) {
        options.message = newest_version_message(options.remoteVersion);
        return false;
    }

    if (installing_stable_over_prerelease(options)) {
        options.message = "Warning: installed version looks experimental/pre; stable release will be installed.";
    } else {
        options.message.clear();
    }
    return true;
}

int run_update_screen(Terminal& terminal, const Options& options) {
    clear_screen();
    banner();
    terminal.restore();

    const UpdateResult result = run_update(options);
    std::cout << '\n';
    if (result == UpdateResult::Updated) {
        std::cout << GREEN << "[+]" << RESET << " KSM updated successfully.\n";
        std::cout << "Run: " << CYAN << "ksm" << RESET << " or " << CYAN << "ksm home" << RESET << '\n';
        std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << '\n';
        return 0;
    }

    if (result == UpdateResult::UpToDate) {
        std::cout << GREEN << "[+]" << RESET << " " << newest_version_message(options.remoteVersion) << '\n';
        std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << '\n';
        return 0;
    }

    std::cout << RED << "[x]" << RESET << " KSM update failed.\n";
    std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << '\n';
    return 1;
}

void refresh_versions(Options& options) {
    options.localVersion = read_local_version();
    options.remoteRelease = {};
    options.remoteVersion.clear();
    if (!options.experimental) {
        options.repoSnapshot = false;
    }

    if (options.repoSnapshot) {
        options.remoteRelease.tag = "main";
        options.remoteRelease.archiveUrl = kRepoArchiveUrl;
        options.remoteRelease.experimental = true;
        options.message = "Repo snapshot selected. VERSION.txt will be read after download.";
        return;
    }

    const auto fetched = fetch_remote_release(options.experimental);
    if (!fetched) {
        options.message = "No " + channel_label(options.experimental) + " found on GitHub Releases.";
        return;
    }

    options.remoteRelease = *fetched;
    options.remoteVersion = fetched->version;

    if (installing_stable_over_prerelease(options)) {
        options.message = "Warning: installed version looks experimental/pre; stable release is available.";
    } else if (!options.force && !options.localVersion.empty() &&
        compare_versions(options.localVersion, options.remoteVersion) >= 0) {
        options.message = newest_version_message(options.remoteVersion);
    } else {
        options.message = "Version info refreshed.";
    }
}

int run_tui(Options options) {
    if (geteuid() != 0) {
        banner();
        std::cerr << RED << "Run with sudo!" << RESET << '\n';
        return 1;
    }

    options.localVersion = read_local_version();
    refresh_versions(options);

    Terminal terminal;
    int cursor = 0;

    while (true) {
        draw(options, cursor);
        const KeyPress key = read_key();

        if (key.key == Key::CtrlC || (key.key == Key::Character && key.value == 'q')) {
            return 0;
        }
        if (key.key == Key::Up) {
            move_cursor(cursor, -1, max_row(options));
        } else if (key.key == Key::Down) {
            move_cursor(cursor, 1, max_row(options));
        } else if (key.key == Key::Tab) {
            cursor = cursor < start_row(options) ? start_row(options) : 0;
        } else if (key.key == Key::Enter) {
            options.message.clear();
            const int currentToolsRow = tools_row(options);
            const int currentStartRow = start_row(options);
            const int currentCancelRow = cancel_row(options);
            if (cursor == 0) {
                refresh_versions(options);
            } else if (cursor == 1) {
                options.experimental = !options.experimental;
                if (!options.experimental) {
                    options.repoSnapshot = false;
                    if (cursor > max_row(options)) cursor = max_row(options);
                }
                refresh_versions(options);
            } else if (options.experimental && cursor == 2) {
                options.repoSnapshot = !options.repoSnapshot;
                refresh_versions(options);
            } else if (cursor == currentToolsRow) {
                options.installMissingTools = !options.installMissingTools;
            } else if (cursor == currentToolsRow + 1) {
                options.preserveConfig = !options.preserveConfig;
            } else if (cursor == currentToolsRow + 2) {
                options.force = !options.force;
                if (options.force && is_newest_version_message(options.message)) {
                    options.message.clear();
                }
            } else if (cursor == currentToolsRow + 3) {
                options.buildProject = !options.buildProject;
            } else if (cursor == currentToolsRow + 4) {
                options.linkCommands = !options.linkCommands;
            } else if (cursor == currentStartRow) {
                if (validate_before_update(options) && confirm_update(options)) {
                    return run_update_screen(terminal, options);
                }
            } else if (cursor == currentCancelRow) {
                return 0;
            }
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
        if (arg == "--force" || arg == "-f") {
            options.force = true;
            continue;
        }
        if (arg == "--experimental" || arg == "-ex") {
            options.experimental = true;
            continue;
        }
        if (arg == "--panel") continue;

        std::cerr << RED << "unknown option:" << RESET << " " << arg << '\n';
        std::cerr << "run 'kupgr --help' to list options.\n";
        return 1;
    }

    return run_tui(options);
}
