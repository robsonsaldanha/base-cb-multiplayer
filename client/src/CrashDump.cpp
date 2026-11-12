#include "CrashDump.hpp"
#include <cbmp/client/Log.hpp>
#include <Windows.h>
#include <DbgHelp.h>
#include <cwchar>
#include <string>

namespace cbmp::client {
namespace {
// Sem heap/locks no filtro: tudo em buffers fixos (nem antes nem depois do dump).
wchar_t g_dumpDir[MAX_PATH * 2] = {};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"cb-mp-%04u%02u%02u-%02u%02u%02u.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    wchar_t path[MAX_PATH * 2 + 64];
    swprintf_s(path, L"%s\\%s", g_dumpDir, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(file);
        // Sem linha de log aqui: o log aloca e trava, e o crash pode ter acontecido com a trava presa ou o heap
        // corrompido. A prova do crash é o próprio .dmp na pasta crash.
    }
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashDump(const std::filesystem::path& dir) {
    wcsncpy_s(g_dumpDir, dir.wstring().c_str(), _TRUNCATE);
    g_previous = SetUnhandledExceptionFilter(&OnCrash);
    Log().Write(Level::Info, std::string("crash handler instalado (filtro anterior: ") + (g_previous ? "sim" : "não") + ")");
}
}  // namespace cbmp::client
