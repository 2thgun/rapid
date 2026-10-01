#pragma once

// Lookups in iRacing's session-info YAML.
//
// The YAML lists every session of the event and every car in it, and the
// telemetry says which entries are the live ones: SessionNum picks the session
// and DriverInfo.DriverCarIdx picks the player's car. Reading the first
// "SessionType", "UserName" or "CarScreenName" in the document gives the first
// session of the weekend (so a race looks like practice) and the first car
// (often the pace car, not the player).
//
// This is not a YAML parser. It only needs "key: value" lines and the "- "
// list items iRacing writes, which it indents consistently.
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rapid::iracing_yaml {

inline std::string trim_value(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n\"'");
    const auto last = value.find_last_not_of(" \t\r\n\"'");
    if (first == std::string::npos) return {};
    return value.substr(first, last - first + 1);
}

// "key: value" -> value when the line's key is exactly `name`. A line that only
// starts with the same letters (CarScreenNameShort for CarScreenName) is not a
// match.
inline bool line_value(const std::string& line, std::string_view name, std::string& value) {
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    if (line.compare(first, 2, "- ") == 0) {
        first = line.find_first_not_of(" \t", first + 1);
        if (first == std::string::npos) return false;
    }
    if (line.compare(first, name.size(), name) != 0) return false;
    auto colon = first + name.size();
    while (colon < line.size() && (line[colon] == ' ' || line[colon] == '\t')) ++colon;
    if (colon >= line.size() || line[colon] != ':') return false;
    value = trim_value(line.substr(colon + 1));
    return true;
}

inline std::vector<std::string> split_lines(const std::string& yaml) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= yaml.size()) {
        const auto end = yaml.find('\n', at);
        lines.push_back(yaml.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return lines;
}

// The first line whose key is `name`, anywhere in the document.
inline std::string first_value(const std::string& yaml, std::string_view name) {
    std::string value;
    for (const auto& line : split_lines(yaml))
        if (line_value(line, name, value)) return value;
    return {};
}

inline std::size_t indent_of(const std::string& line) {
    const auto first = line.find_first_not_of(" \t");
    return first == std::string::npos ? line.size() : first;
}

// The lines under the first "name:" key, as one string: everything indented
// deeper than the key, plus list items written at the key's own indent (iRacing
// writes "Drivers:" and its "- CarIdx" items in the same column). Empty when
// the key is absent.
inline std::string section(const std::string& yaml, std::string_view name) {
    const auto lines = split_lines(yaml);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string value;
        if (!line_value(lines[i], name, value) || !value.empty()) continue;
        const auto key_indent = indent_of(lines[i]);
        std::string text;
        for (std::size_t k = i + 1; k < lines.size(); ++k) {
            const auto indent = indent_of(lines[k]);
            const bool blank = lines[k].find_first_not_of(" \t\r") == std::string::npos;
            const bool item = lines[k].compare(indent, 2, "- ") == 0;
            if (!blank && indent < key_indent) break;
            if (!blank && indent == key_indent && !item) break;
            text += lines[k] + '\n';
        }
        return text;
    }
    return {};
}

// Within the list item whose own key `id_key` equals `id_value`, the value of
// `name`. Only the item's own keys count, not those of lists nested inside it
// (a session's result list has its own CarIdx entries). Empty when no item
// matches or the item has no such key.
inline std::string item_value(const std::string& yaml, std::string_view id_key, const std::string& id_value,
                              std::string_view name) {
    const auto lines = split_lines(yaml);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto dash = lines[i].find_first_not_of(" \t");
        if (dash == std::string::npos || lines[i].compare(dash, 2, "- ") != 0) continue;
        // An item runs until the next line at the dash's indent or shallower.
        std::size_t end = i + 1;
        while (end < lines.size() && (lines[end].find_first_not_of(" \t") == std::string::npos ||
                                      indent_of(lines[end]) > dash))
            ++end;
        // The item's own keys sit two columns past the dash.
        const auto key_indent = dash + 2;
        bool matches = false;
        std::string found;
        bool have = false;
        for (std::size_t k = i; k < end; ++k) {
            if (k != i && indent_of(lines[k]) != key_indent) continue;
            std::string value;
            if (line_value(lines[k], id_key, value) && value == id_value) matches = true;
            if (!have && line_value(lines[k], name, value)) {
                found = value;
                have = true;
            }
        }
        if (matches) return found;
    }
    return {};
}

} // namespace rapid::iracing_yaml
