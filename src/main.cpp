#include "win32.h"

#include <windows.h>

int WINAPI wWinMain(_In_ const HINSTANCE hInstance, _In_opt_ HINSTANCE /*hPrevInstance*/,
                    _In_ LPWSTR /*lpCmdLine*/, _In_ int /*nShowCmd*/) {

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return initGleamingGlass(hInstance);
}