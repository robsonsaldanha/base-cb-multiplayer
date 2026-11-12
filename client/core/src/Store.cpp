#include <cbmp/client/Store.hpp>
#include <algorithm>
#include <cwctype>
#include <string>

namespace cbmp::client {
static std::wstring Lower(std::wstring_view s) {
    std::wstring r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
}

Store DetectStore(std::wstring_view exePath) {
    const std::wstring p = Lower(exePath);
    if (p.find(L"\\steamapps\\") != std::wstring::npos) return Store::Steam;
    if (p.find(L"\\epic games\\") != std::wstring::npos) return Store::Epic;
    return Store::Unknown;
}

const char* StoreName(Store s) {
    switch (s) {
        case Store::Steam: return "Steam";
        case Store::Epic: return "Epic";
        case Store::Unknown: return "desconhecida";
    }
    return "desconhecida";
}
}
