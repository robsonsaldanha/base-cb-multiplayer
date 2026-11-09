#include "CbMpMod.hpp"

#define CBMP_API __declspec(dllexport)
extern "C" {
CBMP_API RC::CppUserModBase* start_mod() { return new CbMpMod(); }
CBMP_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
