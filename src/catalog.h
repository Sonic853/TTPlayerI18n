#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttp::i18n {
struct Message {
    std::string plural;
    std::vector<std::wstring> forms;
};
class Catalog {
public:
    static Catalog Po(std::string_view bytes);
    static Catalog Mo(std::string_view bytes);
    const std::wstring* Lookup(const char* context, std::string_view singular,
                              std::string_view plural, uint64_t count) const;
private:
    void Header(std::string_view text);
    void Add(std::string key, std::string plural, const std::vector<std::string>& forms);
    std::map<std::string, Message, std::less<>> messages_;
    // Resource context -> gettext key. Empty keys mark ambiguous contexts.
    // Own keys so Catalog copies and moves do not leave dangling references.
    std::map<std::string, std::string, std::less<>> resources_;
    unsigned plural_count_{2};
    std::string plural_expression_{"n != 1"};
};
std::wstring NormalizeLanguage(std::wstring_view language);
std::vector<Catalog> LoadCatalogs(const std::filesystem::path& directory,
                                std::wstring_view language);
uint64_t EvaluatePlural(std::string_view expression, uint64_t count);
std::wstring Utf8(std::string_view value);
}
