#pragma once

#include <algorithm>
#include <iostream>
#include <string>

#include "colors.h"

namespace ksm_tui {

inline std::string repeat(char ch, int count) {
    return std::string(std::max(0, count), ch);
}

inline std::string fit(std::string value, size_t width) {
    if (value.size() > width) {
        if (width <= 3) return value.substr(0, width);
        return value.substr(0, width - 3) + "...";
    }
    return value + std::string(width - value.size(), ' ');
}

inline std::string center(const std::string& value, size_t width) {
    if (value.size() >= width) return fit(value, width);
    const size_t left = (width - value.size()) / 2;
    const size_t right = width - value.size() - left;
    return std::string(left, ' ') + value + std::string(right, ' ');
}

inline void clear() {
    std::cout << "\033[2J\033[H";
}

inline void enter_screen() {
    std::cout << "\033[?1049h\033[2J\033[H\033[?25l";
}

inline void leave_screen() {
    std::cout << "\033[?25h\033[?1049l\033[0m" << std::flush;
}

inline void border(int width) {
    std::cout << BLUE << "+" << repeat('-', width - 2) << "+" << RESET << '\n';
}

inline void row(const std::string& text, int width, const std::string& color = "") {
    std::cout << BLUE << "|" << RESET << color << fit(text, width - 2) << RESET
              << BLUE << "|" << RESET << '\n';
}

inline void split_border(int leftWidth, int rightWidth) {
    std::cout << BLUE << "+" << repeat('-', leftWidth - 2) << "+"
              << repeat('-', rightWidth - 1) << "+" << RESET << '\n';
}

inline void split_row(
    const std::string& left,
    const std::string& right,
    int leftWidth,
    int rightWidth,
    const std::string& leftColor = "",
    const std::string& rightColor = ""
) {
    std::cout << BLUE << "|" << RESET << leftColor << fit(left, leftWidth - 2) << RESET
              << BLUE << "|" << RESET << rightColor << fit(right, rightWidth - 1) << RESET
              << BLUE << "|" << RESET << '\n';
}

inline std::string selected(const std::string& text, bool active) {
    return std::string(active ? "> " : "  ") + text;
}

inline std::string checkbox(bool checked) {
    return checked ? GREEN + "[x]" + RESET : DIM + "[ ]" + RESET;
}

} // namespace ksm_tui
