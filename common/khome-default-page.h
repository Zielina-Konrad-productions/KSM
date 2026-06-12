#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace khome_config {

inline std::string compact(std::string line) {
    line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), line.end());
    return line;
}

inline bool contains_enabled_key(const std::string& key) {
    const char* paths[] = {
        "/opt/KSM/kastiusz.conf",
        "kastiusz.conf"
    };

    for (const char* path : paths) {
        std::ifstream conf(path);
        if (!conf.is_open()) {
            continue;
        }

        std::string line;
        while (std::getline(conf, line)) {
            line = compact(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line == key) {
                return true;
            }
        }
    }

    return false;
}

inline bool defaultpage(int page) {
    return contains_enabled_key("khome-default-page-" + std::to_string(page) + "=true");
}

inline bool showallpages() {
    return contains_enabled_key("khome-show-all-pages=true");
}

} // namespace khome_config
