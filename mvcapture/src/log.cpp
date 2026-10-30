#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

namespace mvcapture {
namespace {

constexpr long kMaxLogBytes = 2 * 1024 * 1024;

CRITICAL_SECTION g_lock;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
FILE* g_file = nullptr;
long g_written = 0;

BOOL CALLBACK Init(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    wchar_t base[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        return TRUE;
    }
    wchar_t dir[MAX_PATH]{};
    swprintf(dir, MAX_PATH, L"%ls\\VectorXR", base);
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH]{};
    swprintf(path, MAX_PATH, L"%ls\\mvcapture.log", dir);
    g_file = _wfopen(path, L"w");
    return TRUE;
}

} // namespace

void Log(const char* format, ...) {
    InitOnceExecuteOnce(&g_once, Init, nullptr, nullptr);
    if (!g_file) {
        return;
    }
    char text[1024];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    EnterCriticalSection(&g_lock);
    if (g_written < kMaxLogBytes) {
        g_written += fprintf(g_file, "%02u:%02u:%02u.%03u [%lu] %s\n", now.wHour, now.wMinute, now.wSecond,
                             now.wMilliseconds, GetCurrentThreadId(), text);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace mvcapture
