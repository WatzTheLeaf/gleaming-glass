#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <iterator>

void showFatal(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, args);
    va_end(args);

    MessageBoxW(nullptr, buf, L"gleaming-glass", MB_ICONERROR | MB_OK);
}