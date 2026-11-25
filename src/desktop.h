#pragma once

#include <windows.h>
#include <vector>

HWND AcquireWallpaperHost();

std::vector<RECT> EnumerateMonitors();