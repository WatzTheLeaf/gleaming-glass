#pragma once

#include <windows.h>
#include <wrl/client.h>

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

void showFatal(const wchar_t* fmt, ...);

#define CHECK_HR(expr) do { if (FAILED(expr)) return false; } while (0)