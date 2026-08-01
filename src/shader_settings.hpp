#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace shader_settings {

namespace fs = std::filesystem;

struct Setting {
    std::string name;
    std::string label;
    std::string comment;
    std::string prefix;
    std::string suffix;
    std::vector<std::string> valueTokens;
    std::vector<float> values;
    std::unordered_map<std::string, std::string> valueLabels;
    std::size_t selected = 0;
    std::size_t defaultSelected = 0;
    bool integer = false;
    bool compileTime = false;
};

inline std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

inline bool identifier(std::string_view text) {
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text.front())) ||
                          text.front() == '_')) return false;
    return std::all_of(text.begin() + 1, text.end(), [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
    });
}

inline std::optional<float> number(std::string_view text) {
    const std::string token(text);
    char* end = nullptr;
    const float value = std::strtof(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

inline bool integerToken(std::string_view text) {
    if (text.empty()) return false;
    std::size_t index = text.front() == '+' || text.front() == '-' ? 1 : 0;
    return index < text.size() &&
           std::all_of(text.begin() + static_cast<std::ptrdiff_t>(index), text.end(),
                       [](char character) { return std::isdigit(static_cast<unsigned char>(character)); });
}

inline std::optional<Setting> parseLine(std::string_view line) {
    const std::string stripped = trim(line);
    if (!stripped.starts_with("#define") ||
        (stripped.size() > 7 && !std::isspace(static_cast<unsigned char>(stripped[7])))) {
        return std::nullopt;
    }
    const std::size_t nameStart = stripped.find_first_not_of(" \t", 7);
    if (nameStart == std::string::npos) return std::nullopt;
    const std::size_t nameEnd = stripped.find_first_of(" \t", nameStart);
    if (nameEnd == std::string::npos) return std::nullopt;
    const std::string name = stripped.substr(nameStart, nameEnd - nameStart);
    if (!identifier(name)) return std::nullopt;

    const std::size_t valueStart = stripped.find_first_not_of(" \t", nameEnd);
    if (valueStart == std::string::npos) return std::nullopt;
    const std::size_t valueEnd = stripped.find_first_of(" \t/", valueStart);
    const std::string defaultToken = stripped.substr(valueStart, valueEnd - valueStart);
    const auto defaultValue = number(defaultToken);
    if (!defaultValue) return std::nullopt;

    const std::size_t comment = stripped.find("//", valueStart + defaultToken.size());
    if (comment == std::string::npos) return std::nullopt;
    const std::size_t open = stripped.find('[', comment + 2);
    const std::size_t close = open == std::string::npos ? std::string::npos : stripped.find(']', open + 1);
    if (open == std::string::npos || close == std::string::npos) return std::nullopt;

    Setting setting;
    setting.name = name;
    setting.label = name;
    setting.comment = trim(std::string_view(stripped).substr(close + 1));
    setting.integer = integerToken(defaultToken);
    std::istringstream values(stripped.substr(open + 1, close - open - 1));
    std::string token;
    while (values >> token) {
        const auto parsed = number(token);
        if (!parsed) throw std::runtime_error("invalid shader setting value for " + name + ": " + token);
        setting.integer = setting.integer && integerToken(token);
        setting.valueTokens.push_back(token);
        setting.values.push_back(*parsed);
    }
    if (setting.values.empty()) throw std::runtime_error("shader setting has no values: " + name);
    const auto selected = std::find_if(setting.values.begin(), setting.values.end(), [&](float value) {
        return std::abs(value - *defaultValue) <=
               1.0e-6f * std::max({1.0f, std::abs(value), std::abs(*defaultValue)});
    });
    if (selected == setting.values.end()) {
        throw std::runtime_error("shader setting default is not in its value list: " + name);
    }
    setting.selected = static_cast<std::size_t>(selected - setting.values.begin());
    setting.defaultSelected = setting.selected;
    return setting;
}

inline std::vector<Setting> parse(std::string_view source) {
    std::vector<Setting> settings;
    std::istringstream lines{std::string(source)};
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        try {
            if (auto setting = parseLine(line)) settings.push_back(std::move(*setting));
        } catch (const std::exception& error) {
            throw std::runtime_error("main.glsl:" + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    std::unordered_map<std::string, std::size_t> names;
    for (std::size_t index = 0; index < settings.size(); ++index) {
        if (!names.emplace(settings[index].name, index).second) {
            throw std::runtime_error("duplicate shader setting definition: " + settings[index].name);
        }
    }
    return settings;
}

inline std::unordered_map<std::string, std::string> parseLanguage(std::string_view source) {
    std::unordered_map<std::string, std::string> entries;
    std::istringstream file{std::string(source)};
    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (firstLine && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        firstLine = false;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped.starts_with('#')) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        entries[trim(std::string_view(line).substr(0, separator))] = line.substr(separator + 1);
    }
    return entries;
}

inline std::unordered_map<std::string, std::string> loadLanguage(const fs::path& path) {
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return parseLanguage(std::string(std::istreambuf_iterator<char>(file), {}));
}

inline fs::path englishLanguagePath(const fs::path& shaderRoot) {
    const fs::path languageDirectory = shaderRoot / "lang";
    std::error_code error;
    if (!fs::is_directory(languageDirectory, error) || error) return {};
    for (const fs::directory_entry& entry : fs::directory_iterator(languageDirectory, error)) {
        if (error || !entry.is_regular_file(error)) continue;
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (name == "en_us.lang") return entry.path();
    }
    return {};
}

inline void localize(
    std::vector<Setting>& settings,
    const std::unordered_map<std::string, std::string>& entries) {
    for (Setting& setting : settings) {
        const auto assign = [&](std::string_view key, std::string& output) {
            if (const auto found = entries.find(std::string(key)); found != entries.end()) output = found->second;
        };
        assign("option." + setting.name, setting.label);
        assign("option." + setting.name + ".comment", setting.comment);
        assign("prefix." + setting.name, setting.prefix);
        assign("suffix." + setting.name, setting.suffix);
        for (const std::string& token : setting.valueTokens) {
            if (const auto found = entries.find("value." + setting.name + "." + token);
                found != entries.end()) {
                setting.valueLabels[token] = found->second;
            }
        }
    }
}

inline void localize(std::vector<Setting>& settings, const fs::path& languagePath) {
    localize(settings, loadLanguage(languagePath));
}

inline void preserveSelections(std::vector<Setting>& settings, const std::vector<Setting>& previous) {
    for (Setting& setting : settings) {
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const Setting& candidate) {
            return candidate.name == setting.name && candidate.selected < candidate.valueTokens.size();
        });
        if (old == previous.end()) continue;
        const std::string& token = old->valueTokens[old->selected];
        const auto current = std::find(setting.valueTokens.begin(), setting.valueTokens.end(), token);
        if (current != setting.valueTokens.end()) {
            setting.selected = static_cast<std::size_t>(current - setting.valueTokens.begin());
        }
    }
}

using SavedValues =
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

inline SavedValues parseSavedValues(std::string_view source) {
    SavedValues saved;
    std::istringstream lines{std::string(source)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string shader;
        std::string name;
        std::string token;
        std::string extra;
        if (!(fields >> std::quoted(shader) >> std::quoted(name) >> std::quoted(token)) ||
            fields >> extra) {
            continue;
        }
        saved[shader][name] = token;
    }
    return saved;
}

inline std::string serializeSavedValues(const SavedValues& saved) {
    std::vector<std::tuple<std::string, std::string, std::string>> entries;
    for (const auto& [shader, settings] : saved) {
        for (const auto& [name, token] : settings) {
            entries.emplace_back(shader, name, token);
        }
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream output;
    for (const auto& [shader, name, token] : entries) {
        output << std::quoted(shader) << ' ' << std::quoted(name) << ' '
               << std::quoted(token) << '\n';
    }
    return output.str();
}

inline void applySavedValues(
    std::vector<Setting>& settings, std::string_view shader, const SavedValues& saved) {
    const auto shaderValues = saved.find(std::string(shader));
    if (shaderValues == saved.end()) return;
    for (Setting& setting : settings) {
        const auto savedToken = shaderValues->second.find(setting.name);
        if (savedToken == shaderValues->second.end()) continue;
        const auto token = std::find(
            setting.valueTokens.begin(), setting.valueTokens.end(), savedToken->second);
        if (token != setting.valueTokens.end()) {
            setting.selected = static_cast<std::size_t>(token - setting.valueTokens.begin());
        }
    }
}

inline void updateSavedValues(
    const std::vector<Setting>& settings, std::string_view shader, SavedValues& saved) {
    std::unordered_map<std::string, std::string> shaderValues;
    for (const Setting& setting : settings) {
        if (setting.selected < setting.valueTokens.size() &&
            setting.selected != setting.defaultSelected) {
            shaderValues[setting.name] = setting.valueTokens[setting.selected];
        }
    }
    const std::string shaderId(shader);
    if (shaderValues.empty()) saved.erase(shaderId);
    else saved[shaderId] = std::move(shaderValues);
}

inline bool containsIdentifier(std::string_view text, std::string_view identifierText) {
    std::size_t position = text.find(identifierText);
    while (position != std::string_view::npos) {
        const auto identifierCharacter = [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
        };
        const bool beginsIdentifier = position == 0 || !identifierCharacter(text[position - 1]);
        const std::size_t end = position + identifierText.size();
        const bool endsIdentifier = end == text.size() || !identifierCharacter(text[end]);
        if (beginsIdentifier && endsIdentifier) return true;
        position = text.find(identifierText, position + 1);
    }
    return false;
}

inline std::string patch(std::string_view source, std::vector<Setting>& settings) {
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < settings.size(); ++index) indices[settings[index].name] = index;
    for (Setting& setting : settings) setting.compileTime = false;
    std::istringstream analysis{std::string(source)};
    std::string analysisLine;
    while (std::getline(analysis, analysisLine)) {
        const std::string stripped = trim(analysisLine);
        if (!stripped.starts_with('#') || parseLine(analysisLine)) continue;
        for (Setting& setting : settings) {
            if (containsIdentifier(stripped, setting.name)) setting.compileTime = true;
        }
    }
    static constexpr std::array<std::string_view, 4> components = {"x", "y", "z", "w"};
    std::istringstream lines{std::string(source)};
    std::ostringstream output;
    std::string line;
    while (std::getline(lines, line)) {
        const auto parsed = parseLine(line);
        if (!parsed) {
            output << line << '\n';
            continue;
        }
        const auto found = indices.find(parsed->name);
        if (found == indices.end()) throw std::runtime_error("shader setting patch mismatch: " + parsed->name);
        const std::size_t index = found->second;
        if (settings[index].compileTime) {
            output << "#define " << parsed->name << ' '
                   << settings[index].valueTokens[settings[index].selected] << '\n';
            continue;
        }
        output << "#define " << parsed->name << " (";
        if (settings[index].integer) output << "int(";
        output << "drtBenchSettings.values[" << index / 4 << "]." << components[index % 4];
        if (settings[index].integer) output << ')';
        output << ")\n";
    }
    return output.str();
}

inline std::string uniformDeclaration(std::size_t settingCount) {
    const std::size_t slots = std::max<std::size_t>(1, (settingCount + 3) / 4);
    return "layout(std140, set = 0, binding = 2) uniform DrtBenchSettings {\n"
           "    vec4 values[" + std::to_string(slots) + "];\n"
           "} drtBenchSettings;\n";
}

inline std::vector<float> packedValues(const std::vector<Setting>& settings) {
    std::vector<float> values(std::max<std::size_t>(4, ((settings.size() + 3) / 4) * 4), 0.0f);
    for (std::size_t index = 0; index < settings.size(); ++index) {
        values[index] = settings[index].values[settings[index].selected];
    }
    return values;
}

inline std::string displayValue(const Setting& setting) {
    if (setting.selected >= setting.valueTokens.size()) return {};
    const std::string& token = setting.valueTokens[setting.selected];
    const auto label = setting.valueLabels.find(token);
    return setting.prefix + (label == setting.valueLabels.end() ? token : label->second) + setting.suffix;
}

}
