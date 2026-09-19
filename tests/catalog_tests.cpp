#include "catalog.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>

using namespace ttp::i18n;
namespace {
void Check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class F> void Reject(F&& function) {
    bool failed{}; try { function(); } catch (const std::exception&) { failed = true; }
    Check(failed, "Malformed catalog/expression accepted");
}
std::string Mo(bool big_endian, const std::string& translation = "MO", const std::string& id = "app\4hello") {
    std::string bytes(44, '\0');
    const auto word = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<char>(value >> (8 * (big_endian ? 3 - i : i)));
    };
    word(0, 0x950412de); word(8, 1); word(12, 28); word(16, 36);
    word(28, static_cast<uint32_t>(id.size())); word(32, 44);
    word(36, static_cast<uint32_t>(translation.size())); word(40, static_cast<uint32_t>(45 + id.size()));
    bytes += id; bytes += '\0'; bytes += translation; bytes += '\0';
    return bytes;
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output.write(text.data(), text.size());
}
std::string PoEntry(const std::string& context, const std::string& source, const std::string& translated) {
    return "msgctxt \"" + context + "\"\nmsgid \"" + source + "\"\nmsgstr \"" + translated + "\"\n\n";
}
}
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path temporary;
    try {
        const auto po = Catalog::Po(R"PO(msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"
"Plural-Forms: nplurals=3; plural=n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2;\n"

# context distinguishes equal source strings
msgctxt "verb"
msgid "Open"
msgstr "打开"

msgctxt "adjective"
msgid "Open"
msgstr "开放"

msgctxt ""
msgid "Open"
msgstr "explicit empty context"

msgctxt "app"
msgid "hello"
msgstr "PO"

msgid "escaped"
msgstr "line\n"
"quote \" slash \\ tab\t\101"

#, fuzzy, c-format
msgid "fuzzy"
msgstr "not ready"

#~ msgid "obsolete"
#~ msgstr "old"

msgid "empty"
msgstr ""

msgid "file"
msgid_plural "files"
msgstr[0] "one"
msgstr[1] "few"
msgstr[2] "many"
)PO");
        Check(*po.Lookup("verb", "Open", "", 1) == L"打开", "PO context/UTF-8");
        Check(*po.Lookup("adjective", "Open", "", 1) == L"开放", "PO distinct context");
        Check(!po.Lookup(nullptr, "Open", "", 1), "Context must not leak");
        Check(*po.Lookup("", "Open", "", 1) == L"explicit empty context", "Absent versus explicit empty context");
        Check(!po.Lookup(nullptr, "empty", "", 1) && !po.Lookup(nullptr, "fuzzy", "", 1) &&
              !po.Lookup(nullptr, "obsolete", "", 1), "Untranslated/fuzzy/obsolete fallback");
        Check(*po.Lookup(nullptr, "escaped", "", 1) == L"line\nquote \" slash \\ tab\tA", "PO continuation/escapes");
        Check(*po.Lookup(nullptr, "file", "files", 1) == L"one" &&
              *po.Lookup(nullptr, "file", "files", 22) == L"few" &&
              *po.Lookup(nullptr, "file", "files", 11) == L"many", "Plural forms");
        Check(!po.Lookup(nullptr, "file", "other", 2), "Plural source mismatch");
        for (const auto context : {"ttpres/string/128", "ttpres/menu/1/item/0", "ttpres/dialog/1/title",
                                   "exe/string/4091", "exe/menu/1/item/0", "exe/dialog/1/control/2/0"}) {
            const auto resource = Catalog::Po(PoEntry(context, "打开", "Translated resource"));
            const auto translated = resource.Lookup(context, "開啟", "", 1);
            Check(translated && *translated == L"Translated resource", "PO resource lookup across source languages");
            const auto copied = resource;
            Check(copied.Lookup(context, "other source", "", 1) != nullptr, "Resource index survives Catalog copies");
            Check(!resource.Lookup(context, "", "", 1), "Missing resource must not be synthesized from PO");
            Check(!resource.Lookup(context, "開啟", "plural", 2), "Context fallback is singular only");
            for (const bool big : {false, true}) {
                const auto compiled = Catalog::Mo(Mo(big, "MO resource", std::string(context) + '\4' + "打开"));
                const auto text = compiled.Lookup(context, "開啟", "", 1);
                Check(text && *text == L"MO resource", "MO resource index in both endian formats");
            }
        }
        for (const auto context : {"app", "custom", "ttpres/custom/1", "other/string/1"}) {
            const auto strict = Catalog::Po(PoEntry(context, "original", "translation"));
            Check(!strict.Lookup(context, "changed", "", 1), "Code and unrelated contexts still require exact msgid");
        }
        const auto ambiguous = Catalog::Po(PoEntry("ttpres/string/1", "one", "first") + PoEntry("ttpres/string/1", "two", "second"));
        Check(!ambiguous.Lookup("ttpres/string/1", "third", "", 1), "Ambiguous resource contexts fall back");
        Check(*ambiguous.Lookup("ttpres/string/1", "two", "", 1) == L"second", "Exact msgid resolves ambiguous resource context");
        const auto ignored = Catalog::Po("#, fuzzy\n" + PoEntry("ttpres/string/1", "old", "fuzzy") +
            PoEntry("ttpres/string/1", "current", "ready") + PoEntry("ttpres/string/2", "empty", "") +
            "#~ msgctxt \"ttpres/string/1\"\n#~ msgid \"retired\"\n#~ msgstr \"obsolete\"\n");
        Check(*ignored.Lookup("ttpres/string/1", "different", "", 1) == L"ready", "Fuzzy and obsolete entries do not pollute resource index");
        Check(!ignored.Lookup("ttpres/string/2", "different", "", 1), "Empty resource translation falls back");
        Check(EvaluatePlural("n ? 1 / n : 7", 0) == 7 &&
              EvaluatePlural("n != 0 && 1 / n", 0) == 0 &&
              EvaluatePlural("n == 0 || 1 / n", 0) == 1, "Short circuit");
        Reject([] { EvaluatePlural("n / 0", 3); });
        Reject([] { EvaluatePlural(std::string(100, '(') + "1" + std::string(100, ')'), 1); });
        Reject([] { EvaluatePlural("n ? 1", 1); });
        for (const bool big : {false, true}) {
            const auto mo = Catalog::Mo(Mo(big));
            Check(*mo.Lookup("app", "hello", "", 1) == L"MO", "MO endian/context");
        }
        for (size_t length = 0; length < Mo(false).size(); ++length)
            Reject([&] { Catalog::Mo(Mo(false).substr(0, length)); });
        auto corrupt = Mo(false); corrupt[12] = static_cast<char>(0xff);
        Reject([&] { Catalog::Mo(corrupt); });
        Reject([] { Catalog::Po("msgid \"x\"\nmsgstr \"unterminated\n"); });
        Reject([] { Catalog::Po("msgid \"x\"\nmsgstr \"one\"\n\nmsgid \"x\"\nmsgstr \"two\"\n"); });
        Reject([] { Catalog::Po("msgid \"x\"\nmsgstr \"\xff\"\n"); });
        Reject([] { Catalog::Po("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=GBK\\n\"\n"); });
        Check(NormalizeLanguage(L"en-US.UTF-8") == L"en_US" &&
              NormalizeLanguage(L"../../outside").empty() &&
              NormalizeLanguage(L"en/US").empty(), "Locale/path validation");

        temporary = std::filesystem::temp_directory_path() /
            (L"ttp-i18n-catalog-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount()));
        const auto base = temporary / L"en_US";
        Write(base / L"ttplayer.po", "msgctxt \"app\"\nmsgid \"hello\"\nmsgstr \"PO\"\n");
        auto catalogs = LoadCatalogs(temporary, L"en-US");
        Check(catalogs.size() == 1 && *catalogs[0].Lookup("app", "hello", "", 1) == L"PO", "PO-only loading");
        Write(base / L"ttplayer.mo", Mo(false));
        catalogs = LoadCatalogs(temporary, L"en_US");
        Check(catalogs.size() == 1 && *catalogs[0].Lookup("app", "hello", "", 1) == L"MO", "MO priority");
        Write(base / L"ttplayer.mo", "broken");
        catalogs = LoadCatalogs(temporary, L"en_US");
        Check(catalogs.size() == 1 && *catalogs[0].Lookup("app", "hello", "", 1) == L"PO", "Invalid MO -> PO");
        Write(temporary / L"en/ttplayer.po", "msgid \"parent\"\nmsgstr \"language fallback\"\n");
        catalogs = LoadCatalogs(temporary, L"en_US");
        Check(catalogs.size() == 2 && *catalogs[1].Lookup(nullptr, "parent", "", 1) == L"language fallback", "Parent locale");
        Write(temporary / L"chs/ttplayer.po", "msgctxt \"app\"\nmsgid \"hello\"\nmsgstr \"simplified\"\n");
        Write(temporary / L"cht/ttplayer.po", "msgctxt \"app\"\nmsgid \"hello\"\nmsgstr \"traditional\"\n");
        for (const auto language : {L"chs", L"CHS", L"zh_CN", L"zh-SG.UTF-8", L"zh-Hans", L"zh-Hans-TW", L"zh_CHS"}) {
            catalogs = LoadCatalogs(temporary, language);
            Check(catalogs.size() == 1 && *catalogs[0].Lookup("app", "hello", "", 1) == L"simplified", "Simplified Chinese aliases");
        }
        for (const auto language : {L"cht", L"zh_TW", L"zh_HK", L"zh_MO", L"zh-Hant-CN", L"zh_CHT", L"zh_TW@custom"}) {
            catalogs = LoadCatalogs(temporary, language);
            Check(catalogs.size() == 1 && *catalogs[0].Lookup("app", "hello", "", 1) == L"traditional", "Traditional Chinese aliases");
        }
        Write(temporary / L"zh_CN/ttplayer.po", "msgctxt \"app\"\nmsgid \"hello\"\nmsgstr \"exact PO\"\n");
        Write(temporary / L"zh_CN/ttplayer.mo", Mo(false, "exact MO"));
        Write(temporary / L"zh/ttplayer.po", "msgctxt \"app\"\nmsgid \"hello\"\nmsgstr \"generic\"\n");
        catalogs = LoadCatalogs(temporary, L"zh_CN");
        Check(catalogs.size() == 3 && *catalogs[0].Lookup("app", "hello", "", 1) == L"exact MO" &&
              *catalogs[1].Lookup("app", "hello", "", 1) == L"simplified" &&
              *catalogs[2].Lookup("app", "hello", "", 1) == L"generic", "Exact MO, Chinese alias, generic locale priority");
        catalogs = LoadCatalogs(temporary, L"zh_TW");
        Check(catalogs.size() == 2 && *catalogs[0].Lookup("app", "hello", "", 1) == L"traditional", "Traditional fallback does not select chs");
        if (argc > 1) {
            const auto matches = [](const Catalog& catalog, const char* context, const char* source,
                                    const wchar_t* expected, const char* plural = "", uint64_t count = 1) {
                const auto text = catalog.Lookup(context, source, plural, count);
                return text && *text == expected;
            };
            for (const auto language : {L"chs", L"zh_CN", L"cht", L"zh_TW", L"zh_HK"}) {
                catalogs = LoadCatalogs(argv[1], language);
                Check(catalogs.size() == 1, "Shipped Chinese catalog loads through locale aliases");
                const bool traditional = std::wstring_view(language) != L"chs" && std::wstring_view(language) != L"zh_CN";
                Check(matches(catalogs[0], "app", "Error in ttpcomm.dll, please resetup this program!",
                    traditional ? L"ttpcomm.dll 載入失敗，請重新安裝播放器！" : L"ttpcomm.dll 加载失败，请重新安装播放器！"),
                    "Shipped startup error translation");
                Check(matches(catalogs[0], "exe/string/4091", "屏幕选择", traditional ? L"屏幕選擇" : L"屏幕选择"),
                    "Shipped EXE translation");
                Check(matches(catalogs[0], "ttpres/string/33146", "正在服务器上搜索歌词...",
                    traditional ? L"正在伺服器上搜尋歌詞..." : L"正在服务器上搜索歌词..."), "Imported resource translation");
                for (const auto count : {0, 1, 2, 100})
                    Check(matches(catalogs[0], "app/search/count", "%u 个", traditional ? L"%u 個" : L"%u 个", "%u 个", count),
                        "Shipped Chinese single plural form");
            }
        }
        std::filesystem::remove_all(temporary);
        std::cout << "gettext catalog tests passed\n"; return 0;
    } catch (const std::exception& error) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove_all(temporary, ignored); }
        std::cerr << error.what() << '\n'; return 1;
    }
}
