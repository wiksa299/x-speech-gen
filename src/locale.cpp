#include "locale.hpp"
#include "english.hpp"

#include <XPLMUtilities.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <set>
#include <string_view>

namespace xsg {
namespace {
const char* simulator_language() {
    switch (XPLMGetLanguage()) {
    case xplm_Language_French: return "fr";
    case xplm_Language_Spanish: return "es";
    case xplm_Language_German: return "de";
    case xplm_Language_Italian: return "it";
    case xplm_Language_Korean: return "ko";
    case xplm_Language_Russian: return "ru";
    case xplm_Language_Greek: return "el";
    case xplm_Language_Japanese: return "ja";
    case xplm_Language_Chinese: return "zh";
    case xplm_Language_Ukrainian: return "uk";
    default: return "en";
    }
}

std::multiset<std::string> parameters(const std::string& value) {
    std::multiset<std::string> result;
    std::size_t pos = 0;
    while ((pos = value.find_first_of("{}", pos)) != std::string::npos) {
        const auto end = value.find('}', pos + 1);
        if (value[pos] != '{' || end == std::string::npos) throw std::runtime_error("invalid placeholder");
        result.insert(value.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return result;
}
}

std::filesystem::path interface_preferences_path() {
    char path[2048]{};
    XPLMGetPrefsPath(path);
    return std::filesystem::u8path(path).parent_path() / "X-Speech-Gen-ui.json";
}

int read_interface_language(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    char bytes[1024];
    if (!file || file.read(bytes, sizeof(bytes)) || !file.eof() || file.bad()) return 0;
    const auto settings = nlohmann::json::parse(bytes, bytes + file.gcount(), nullptr, false);
    if (!settings.is_object()) return 0;
    const auto value = settings.find("interface_language");
    if (value == settings.end() || !value->is_string()) return 0;
    const auto language = value->get_ref<const std::string&>();
    const auto found = std::find(interface_languages.begin(), interface_languages.end(), language);
    return found == interface_languages.end() ? 0 : int(found - interface_languages.begin());
}

bool save_interface_language(const std::filesystem::path& path, int selection) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << nlohmann::json{{"interface_language", interface_languages.at(selection)}}.dump(2) << '\n';
    file.close();
    std::error_code error;
    if (file) {
        std::filesystem::rename(temporary, path, error);
        if (!error) return true;
    }
    std::filesystem::remove(temporary, error);
    return false;
}

void Locale::load(const std::filesystem::path& root, int selection) {
    using Json = nlohmann::json;
    const auto english = Json::parse(english_json);
    auto strings = english;
    const auto overlay = [&](const char* code) {
        const auto path = root / "locales" / (std::string(code) + ".json");
        try {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("cannot open file");
            std::set<std::string> keys;
            const auto json = Json::parse(file, [&](int, Json::parse_event_t event, Json& item) {
                if (event == Json::parse_event_t::key && !keys.insert(item.get<std::string>()).second)
                    throw std::runtime_error("duplicate key");
                return true;
            });
            if (!json.is_object()) throw std::runtime_error("expected an object");
            for (const auto& [key, item] : json.items()) {
                if (!english.contains(key)) continue;
                const auto& value = item.get_ref<const std::string&>();
                if (value.empty() || value.size() > 1024 || value.find("##") != std::string::npos ||
                    std::any_of(value.begin(), value.end(), [](unsigned char c){ return c < 32; }) ||
                    parameters(value) != parameters(english.at(key).get_ref<const std::string&>()))
                    throw std::runtime_error("invalid translation for " + key);
            }
            for (const auto& [key, item] : json.items()) if (english.contains(key)) strings[key] = item;
        } catch (const std::exception& error) {
            const auto message = "X-Speech Gen: " + path.u8string() + ": " + error.what() + "\n";
            XPLMDebugString(message.c_str());
        }
    };
    overlay("en");
    const std::string_view code = selection == 0 ? simulator_language() : interface_languages.at(selection);
    if (code != "en") overlay(code.data());
    entries_.clear();
    for (const auto& [key, item] : strings.items()) {
        const auto& value = item.get_ref<const std::string&>();
        entries_.emplace(key, Entry{value, value + "###" + key});
    }
}

const Locale::Entry& Locale::entry(const char* key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) throw std::logic_error(std::string("Unknown translation key ") + key);
    return found->second;
}

const char* Locale::text(const char* key) const { return entry(key).text.c_str(); }
const char* Locale::label(const char* key) const { return entry(key).label.c_str(); }

std::string Locale::format(const char* key, std::initializer_list<std::pair<std::string, std::string>> args) const {
    const auto& pattern = entry(key).text;
    std::string result;
    std::size_t offset = 0;
    for (auto pos = pattern.find('{'); pos != std::string::npos; pos = pattern.find('{', offset)) {
        const auto end = pattern.find('}', pos + 1);
        const auto name = pattern.substr(pos + 1, end - pos - 1);
        result.append(pattern, offset, pos - offset);
        const auto arg = std::find_if(args.begin(), args.end(), [&](const auto& pair){ return pair.first == name; });
        if (arg == args.end()) throw std::logic_error("Missing translation argument " + name);
        result += arg->second;
        offset = end + 1;
    }
    return result.append(pattern, offset);
}

std::map<std::string, std::string> Locale::engine_strings() const {
    std::map<std::string, std::string> strings;
    for (const auto& [key, entry] : entries_)
        if (key.compare(0, 6, "error.") == 0 || key.compare(0, 7, "status.") == 0 || key == "folder.picker")
            strings.emplace(key, entry.text);
    return strings;
}
}
