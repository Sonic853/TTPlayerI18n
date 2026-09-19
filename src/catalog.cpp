#include "catalog.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace ttp::i18n {
namespace {
constexpr size_t kMaxCatalog = 16 * 1024 * 1024;
constexpr size_t kMaxMessages = 100000;
[[noreturn]] void Invalid() { throw std::runtime_error("Invalid gettext catalog"); }
std::string_view Trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == s.npos ? std::string_view{} : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Lower(std::string_view s) {
    std::string result(s);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
std::string Key(const char* context, std::string_view id) {
    return !context ? std::string(id) : std::string(context) + '\4' + std::string(id);
}
bool ResourceContext(std::string_view context) {
    if (context.starts_with("ttpres/")) context.remove_prefix(7);
    else if (context.starts_with("exe/")) context.remove_prefix(4);
    else return false;
    return context.starts_with("string/") || context.starts_with("dialog/") || context.starts_with("menu/");
}

// The gettext plural expression grammar. Evaluation is bounded and uses
// unsigned arithmetic; &&, || and ?: short-circuit (including division).
class Expression {
    std::string_view text_;
    size_t at_{};
    uint64_t n_{};
    unsigned depth_{};
    void Space() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t')) ++at_; }
    bool Eat(std::string_view token) {
        Space();
        if (text_.substr(at_).starts_with(token)) { at_ += token.size(); return true; }
        return false;
    }
    uint64_t Atom(bool eval) {
        if (++depth_ > 64) Invalid();
        uint64_t v{};
        if (Eat("!")) v = !Atom(eval);
        else if (Eat("-")) v = uint64_t{} - Atom(eval);
        else if (Eat("+")) v = Atom(eval);
        else if (Eat("(")) { v = Conditional(eval); if (!Eat(")")) Invalid(); }
        else if (Eat("n")) v = n_;
        else {
            Space();
            auto result = std::from_chars(text_.data() + at_, text_.data() + text_.size(), v);
            if (result.ec != std::errc{} || result.ptr == text_.data() + at_) Invalid();
            at_ = static_cast<size_t>(result.ptr - text_.data());
        }
        --depth_;
        return eval ? v : 0;
    }
    static int Precedence(std::string_view op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 0;
    }
    uint64_t Binary(int minimum, bool eval) {
        uint64_t left = Atom(eval);
        for (;;) {
            Space();
            auto op = text_.substr(at_, 2);
            if (!Precedence(op)) op = text_.substr(at_, 1);
            const int priority = Precedence(op);
            if (priority < minimum) return left;
            at_ += op.size();
            const bool right_eval = eval && !(op == "&&" && !left) && !(op == "||" && left);
            const auto right = Binary(priority + 1, right_eval);
            if (!eval) continue;
            if (op == "||") left = left || right;
            else if (op == "&&") left = left && right;
            else if (op == "==") left = left == right;
            else if (op == "!=") left = left != right;
            else if (op == "<") left = left < right;
            else if (op == "<=") left = left <= right;
            else if (op == ">") left = left > right;
            else if (op == ">=") left = left >= right;
            else if (op == "+") left += right;
            else if (op == "-") left -= right;
            else if (op == "*") left *= right;
            else { if (!right) Invalid(); left = op == "/" ? left / right : left % right; }
        }
    }
    uint64_t Conditional(bool eval) {
        if (++depth_ > 64) Invalid();
        auto condition = Binary(1, eval);
        if (Eat("?")) {
            const auto yes = Conditional(eval && condition);
            if (!Eat(":")) Invalid();
            const auto no = Conditional(eval && !condition);
            condition = condition ? yes : no;
        }
        --depth_;
        return condition;
    }
public:
    Expression(std::string_view text, uint64_t n) : text_(text), n_(n) {}
    uint64_t Run() {
        if (text_.empty() || text_.size() > 1024) Invalid();
        auto value = Conditional(true);
        Space(); if (at_ != text_.size()) Invalid();
        return value;
    }
};

std::string Quoted(std::string_view line) {
    line = Trim(line);
    if (line.size() < 2 || line.front() != '"') Invalid();
    std::string result;
    for (size_t i = 1; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') { if (!Trim(line.substr(i + 1)).empty()) Invalid(); return result; }
        if (c == '\\') {
            if (++i >= line.size()) Invalid();
            c = line[i];
            switch (c) {
            case 'a': c = '\a'; break; case 'b': c = '\b'; break;
            case 'f': c = '\f'; break; case 'n': c = '\n'; break;
            case 'r': c = '\r'; break; case 't': c = '\t'; break;
            case 'v': c = '\v'; break; case '\\': case '"': break;
            case 'x': {
                const size_t begin = i + 1;
                unsigned value{};
                while (i + 1 < line.size()) {
                    const char next = line[i + 1];
                    const int digit = next >= '0' && next <= '9' ? next - '0' :
                        next >= 'a' && next <= 'f' ? next - 'a' + 10 :
                        next >= 'A' && next <= 'F' ? next - 'A' + 10 : -1;
                    if (digit < 0) break;
                    value = value * 16 + digit;
                    if (value > 255) Invalid();
                    ++i;
                }
                if (i < begin) Invalid();
                c = static_cast<char>(value); break;
            }
            default: {
                if (c < '0' || c > '7') Invalid();
                unsigned value = c - '0';
                for (int digit = 1; digit < 3 && i + 1 < line.size() &&
                     line[i + 1] >= '0' && line[i + 1] <= '7'; ++digit)
                    value = value * 8 + (line[++i] - '0');
                if (value > 255) Invalid();
                c = static_cast<char>(value);
            }
            }
        }
        if (!c || c == '\4') Invalid();
        result += c;
    }
    Invalid();
}
std::vector<std::string> Split(std::string_view s, char delimiter) {
    std::vector<std::string> result;
    for (;;) {
        auto end = s.find(delimiter);
        result.emplace_back(s.substr(0, end));
        if (end == s.npos) return result;
        s.remove_prefix(end + 1);
    }
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) Invalid();
    const auto size = file.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(kMaxCatalog)) Invalid();
    std::string bytes(static_cast<size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) Invalid();
    return bytes;
}
}

std::wstring Utf8(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > kMaxCatalog) Invalid();
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
    if (!size) Invalid();
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                       static_cast<int>(value.size()), result.data(), size);
    return result;
}
uint64_t EvaluatePlural(std::string_view expression, uint64_t count) {
    return Expression(expression, count).Run();
}
void Catalog::Header(std::string_view text) {
    if (text.find('\0') != text.npos) Invalid();
    static_cast<void>(Utf8(text));
    const auto lower = Lower(text);
    if (const auto at = lower.find("charset="); at != std::string::npos) {
        auto charset = Trim(std::string_view(lower).substr(at + 8));
        charset = charset.substr(0, charset.find_first_of("; \t\r\n"));
        if (charset != "utf-8" && charset != "utf8") Invalid();
    }
    for (const auto& line : Split(text, '\n')) {
        if (!Lower(line).starts_with("plural-forms:")) continue;
        bool have_count = false, have_expression = false;
        for (const auto& field : Split(std::string_view(line).substr(13), ';')) {
            auto value = Trim(field);
            const auto equal = value.find('=');
            if (equal == value.npos) continue;
            const auto name = Trim(value.substr(0, equal));
            value = Trim(value.substr(equal + 1));
            if (name == "nplurals") {
                unsigned count{};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !count || count > 16) Invalid();
                plural_count_ = count; have_count = true;
            } else if (name == "plural") {
                plural_expression_ = value; have_expression = true;
                static_cast<void>(EvaluatePlural(value, 1));
            }
        }
        if (!have_count || !have_expression) Invalid();
    }
}
void Catalog::Add(std::string key, std::string plural, const std::vector<std::string>& forms) {
    if (messages_.size() >= kMaxMessages || forms.empty() || forms.size() > 16) Invalid();
    static_cast<void>(Utf8(key)); static_cast<void>(Utf8(plural));
    Message message{std::move(plural), {}};
    for (const auto& form : forms) message.forms.push_back(Utf8(form));
    const auto [at, inserted] = messages_.emplace(std::move(key), std::move(message));
    if (!inserted) Invalid();
    const auto separator = at->first.find('\4');
    if (separator != at->first.npos && at->second.plural.empty() && separator + 1 < at->first.size()) {
        const auto context = at->first.substr(0, separator);
        if (ResourceContext(context)) {
            const auto [resource, unique] = resources_.emplace(context, at->first);
            // Different active msgids for one resource ID must not pick an
            // arbitrary translation when the loaded DLL has a third wording.
            if (!unique) resource->second.clear();
        }
    }
}

Catalog Catalog::Po(std::string_view bytes) {
    if (bytes.size() > kMaxCatalog) Invalid();
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    static_cast<void>(Utf8(bytes));
    Catalog result;
    struct Entry {
        std::optional<std::string> context, id, plural;
        std::map<unsigned, std::string> forms;
        bool fuzzy{};
    } entry;
    std::string* continuation{};
    const auto flush = [&] {
        if (entry.id) {
            if (entry.forms.empty()) Invalid();
            if (!entry.fuzzy) {
                std::vector<std::string> forms(entry.forms.rbegin()->first + 1);
                for (auto& [index, value] : entry.forms) forms[index] = value;
                if (entry.id->empty() && !entry.context) result.Header(forms.front());
                else result.Add(entry.context ? *entry.context + '\4' + *entry.id : *entry.id,
                                entry.plural.value_or(""), forms);
            }
        } else if (entry.context || entry.plural || !entry.forms.empty()) Invalid();
        entry = {}; continuation = nullptr;
    };
    for (const auto& raw : Split(bytes, '\n')) {
        auto line = Trim(raw);
        if (line.empty()) { flush(); continue; }
        if (line.front() == '#') {
            if (!entry.forms.empty()) flush();
            if (line.starts_with("#,")) {
                for (const auto& flag : Split(line.substr(2), ','))
                    if (Trim(flag) == "fuzzy") entry.fuzzy = true;
            }
            continue; // Translator comments, previous and obsolete entries.
        }
        if (line.front() == '"') {
            if (!continuation) Invalid();
            *continuation += Quoted(line); continue;
        }
        const auto space = line.find_first_of(" \t");
        if (space == line.npos) Invalid();
        const auto keyword = line.substr(0, space);
        const auto value = Quoted(line.substr(space));
        if (keyword == "msgctxt") {
            if (entry.id) flush();
            if (entry.context) Invalid();
            entry.context = value; continuation = &*entry.context;
        } else if (keyword == "msgid") {
            if (entry.id) flush();
            entry.id = value; continuation = &*entry.id;
        } else if (keyword == "msgid_plural") {
            if (!entry.id || entry.plural || !entry.forms.empty()) Invalid();
            entry.plural = value; continuation = &*entry.plural;
        } else if (keyword == "msgstr" || keyword.starts_with("msgstr[")) {
            if (!entry.id) Invalid();
            unsigned index{};
            if (keyword != "msgstr") {
                if (!entry.plural || !keyword.ends_with(']')) Invalid();
                const auto number = keyword.substr(7, keyword.size() - 8);
                auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || index >= 16) Invalid();
            } else if (entry.plural) Invalid();
            auto [at, inserted] = entry.forms.emplace(index, value);
            if (!inserted) Invalid();
            continuation = &at->second;
        } else Invalid();
    }
    flush();
    return result;
}

Catalog Catalog::Mo(std::string_view bytes) {
    if (bytes.size() < 28 || bytes.size() > kMaxCatalog) Invalid();
    const auto little = [&](size_t at) -> uint32_t {
        if (at > bytes.size() - 4) Invalid();
        uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
        return value;
    };
    const auto magic = little(0);
    if (magic != 0x950412de && magic != 0xde120495) Invalid();
    const auto word = [&](size_t at) {
        const auto v = little(at);
        return magic == 0x950412de ? v : (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
    };
    if (word(4) != 0) Invalid(); // Unsupported revisions fall back to PO/source.
    const size_t count = word(8), originals = word(12), translations = word(16);
    if (count > kMaxMessages || originals > bytes.size() || translations > bytes.size() ||
        count > (bytes.size() - originals) / 8 || count > (bytes.size() - translations) / 8) Invalid();
    const auto string = [&](size_t at) {
        const size_t length = word(at), offset = word(at + 4);
        if (offset >= bytes.size() || length >= bytes.size() - offset || bytes[offset + length]) Invalid();
        return bytes.substr(offset, length);
    };
    Catalog result;
    for (size_t i = 0; i < count; ++i) {
        auto id = string(originals + i * 8), translated = string(translations + i * 8);
        if (id.empty()) { result.Header(translated); continue; }
        const auto nul = id.find('\0');
        auto plural = nul == id.npos ? std::string_view{} : id.substr(nul + 1);
        if (plural.find('\0') != plural.npos) Invalid();
        result.Add(std::string(id.substr(0, nul)), std::string(plural), Split(translated, '\0'));
    }
    return result;
}

const std::wstring* Catalog::Lookup(const char* context, std::string_view singular,
                                   std::string_view plural, uint64_t count) const {
    auto found = messages_.find(Key(context, singular));
    if (found == messages_.end() && context && !singular.empty() && plural.empty()) {
        const auto resource = resources_.find(context);
        if (resource != resources_.end() && !resource->second.empty()) found = messages_.find(resource->second);
    }
    if (found == messages_.end() || found->second.plural != plural) return nullptr;
    uint64_t index{};
    if (!plural.empty()) index = EvaluatePlural(plural_expression_, count);
    if (index >= plural_count_ || index >= found->second.forms.size()) return nullptr;
    const auto& text = found->second.forms[static_cast<size_t>(index)];
    return text.empty() ? nullptr : &text;
}

std::wstring NormalizeLanguage(std::wstring_view language) {
    if (language.empty() || language.size() > 64) return {};
    if (const auto dot = language.find(L'.'); dot != language.npos) language = language.substr(0, dot);
    std::wstring result(language);
    bool territory{};
    for (auto& c : result) {
        if (c == L'-') c = L'_';
        if (c == L'_' || c == L'@') { territory = c == L'_'; continue; }
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))) return {};
        if (territory && c >= L'a' && c <= L'z') c -= L'a' - L'A';
        else if (!territory && c >= L'A' && c <= L'Z') c += L'a' - L'A';
    }
    if (result.empty() || result.front() == L'_' || result.front() == L'@') return {};
    return result;
}
std::vector<Catalog> LoadCatalogs(const std::filesystem::path& directory,
                                std::wstring_view language) {
    auto locale = NormalizeLanguage(language);
    std::wstring alias;
    const auto chinese = locale.substr(0, locale.find(L'@'));
    if (chinese.starts_with(L"zh_")) {
        // Script wins over territory (for example zh-Hant-CN). Exact locale
        // catalogs remain first; chs/cht precede the generic zh fallback.
        const auto variant = chinese.substr(3);
        if (variant == L"HANS" || variant.starts_with(L"HANS_") || variant == L"CHS") alias = L"chs";
        else if (variant == L"HANT" || variant.starts_with(L"HANT_") || variant == L"CHT") alias = L"cht";
        else if (variant == L"CN" || variant == L"SG") alias = L"chs";
        else if (variant == L"TW" || variant == L"HK" || variant == L"MO") alias = L"cht";
    }
    std::vector<Catalog> result;
    const auto append = [&](std::wstring_view name) {
        const auto base = directory / name / L"LC_MESSAGES" / L"ttplayer";
        bool loaded{};
        try { result.push_back(Catalog::Mo(Read(base.wstring() + L".mo"))); loaded = true; } catch (...) {}
        if (!loaded) try { result.push_back(Catalog::Po(Read(base.wstring() + L".po"))); } catch (...) {}
    };
    while (!locale.empty()) {
        if (locale == L"zh" && !alias.empty()) append(alias);
        append(locale);
        auto end = locale.find_last_of(L"_@");
        if (end == locale.npos) break;
        locale.resize(end);
    }
    return result;
}
}
