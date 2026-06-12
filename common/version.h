#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace ksm_version {

inline std::string trim(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), value.end());
    return value;
}

inline std::string read_version_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }

    std::string version;
    file >> version;
    return trim(version);
}

inline std::string version() {
    std::string version = read_version_file("/opt/KSM/VERSION.txt");
    if (version.empty()) {
        version = read_version_file("VERSION.txt");
    }
    return version.empty() ? "unknown" : version;
}

} // namespace ksm_version
