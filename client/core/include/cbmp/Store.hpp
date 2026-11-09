#pragma once
#include <string_view>

namespace cbmp {
enum class Store { Steam, Epic, Unknown };
Store DetectStore(std::wstring_view exePath); // heurística pelo caminho
const char* StoreName(Store s);
}
