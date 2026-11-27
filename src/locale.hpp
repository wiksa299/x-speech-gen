#pragma once

#include <filesystem>
#include <array>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

namespace xsg {
inline constexpr char diagnostic_signature[] = "wiksa.x-speech-gen-diagnostic";
inline constexpr int locale_changed_message = 0x58534713;
inline constexpr std::array interface_languages{"auto", "en", "fr", "es"};

std::filesystem::path interface_preferences_path();
int read_interface_language(const std::filesystem::path& path);
bool save_interface_language(const std::filesystem::path& path, int selection);

class Locale {
public:
    void load(const std::filesystem::path& provider_root, int selection);
    const char* text(const char* key) const;
    const char* label(const char* key) const;
    std::string format(const char* key, std::initializer_list<std::pair<std::string, std::string>> args) const;
    std::map<std::string, std::string> engine_strings() const;
private:
    struct Entry { std::string text, label; };
    const Entry& entry(const char* key) const;
    std::map<std::string, Entry, std::less<>> entries_;
};
}
