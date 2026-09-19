#include "catalog.h"
#include "ttplayer/i18n/api.h"
#include <algorithm>
#include <limits>

namespace {
using Catalogs = std::vector<ttp::i18n::Catalog>;
void* TTP_I18N_CALL Open(const wchar_t* directory, const wchar_t* language) {
    try {
        if (!directory || !language) return nullptr;
        auto catalogs = ttp::i18n::LoadCatalogs(directory, language);
        return catalogs.empty() ? nullptr : new Catalogs(std::move(catalogs));
    } catch (...) { return nullptr; }
}
void TTP_I18N_CALL Close(void* catalog) { delete static_cast<Catalogs*>(catalog); }
uint32_t TTP_I18N_CALL Lookup(const void* handle, const char* context,
    const char* singular, const char* plural, uint64_t count, wchar_t* output, uint32_t capacity) {
    try {
        if (!handle || !singular) return 0;
        for (const auto& catalog : *static_cast<const Catalogs*>(handle)) {
            const auto* text = catalog.Lookup(context, singular, plural ? plural : "", count);
            if (!text) continue;
            if (text->size() >= std::numeric_limits<uint32_t>::max()) return 0;
            const auto needed = static_cast<uint32_t>(text->size() + 1);
            if (output && capacity >= needed) std::copy_n(text->c_str(), needed, output);
            return needed;
        }
    } catch (...) {}
    return 0;
}
const TtpI18nApi api{sizeof(TtpI18nApi), TTP_I18N_ABI_VERSION, Open, Close, Lookup};
}
extern "C" const TtpI18nApi* TTP_I18N_CALL TtpI18n_GetApi(uint32_t version) {
    return version == TTP_I18N_ABI_VERSION ? &api : nullptr;
}
