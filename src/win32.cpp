#include "win32.h"

#include "common.h"
#include "desktop.h"
#include "renderer.h"
#include "video.h"

#include <objbase.h>
#include <shellapi.h>
#include <mfapi.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace {

    enum class Action {
        Run, Stop,
    };

    struct Options {
        std::wstring video;
        Action action = Action::Run;
    };

    struct App {
        HINSTANCE instance = nullptr;
        HWND control = nullptr;
        HWND host = nullptr;
        std::vector<HWND> outputs;
        Options options;
        VideoPlayer video;
        UINT taskbarCreatedMessage = 0;
        bool needRebuild = false;
        bool running = true;
        Renderer renderer;
    };

    App mainApp;
    constexpr wchar_t kControlClass[] = L"GleamingGlassControl";
    constexpr wchar_t kOutputClass[] = L"GleamingGlassOutput";

    constexpr LONGLONG kUnitsPerSecond = 10000000;

    HANDLE g_timer = nullptr;
    LONGLONG g_qpcFrequency = 1;

    LONGLONG qpcNow() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    LONGLONG rescale(const LONGLONG value, const LONGLONG numerator, const LONGLONG denominator) {
        return value / denominator * numerator + (value % denominator) * numerator / denominator;
    }

    struct MediaClock {
        LONGLONG base = 0;
        bool needsRebase = true;

        [[nodiscard]] LONGLONG now() const { return rescale(qpcNow() - base, kUnitsPerSecond, g_qpcFrequency); }

        void rebaseTo(const LONGLONG timestamp) {
            base = qpcNow() - rescale(timestamp, g_qpcFrequency, kUnitsPerSecond);
            needsRebase = false;
        }
    };

    void waitOrPumpMessages(const DWORD milliseconds) {
        if (g_timer != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
            SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE const handles[1] = {g_timer};
            MsgWaitForMultipleObjectsEx(1, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    bool pumpMessages() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return mainApp.running;
    }

    bool shouldSuspend() {
        QUERY_USER_NOTIFICATION_STATE state;
        if (FAILED(SHQueryUserNotificationState(&state)))
            return false;

        switch (state) {
            case QUNS_NOT_PRESENT:
            case QUNS_BUSY:
            case QUNS_RUNNING_D3D_FULL_SCREEN:
            case QUNS_PRESENTATION_MODE:
                return true;
            default:
                return false;
        }
    }

    bool parseArguments(Options &options) {
        int count = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (argv == nullptr) return false;
        bool areArgsOk = true;
        for (int i = 1; i < count && areArgsOk; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--stop") {
                options.action = Action::Stop;
            } else if (options.video.empty() && (arg.empty() || arg[0] != L'-')) {
                wchar_t full[MAX_PATH * 4];
                if (GetFullPathNameW(arg.c_str(), std::size(full), full, nullptr) != 0)
                    options.video = full;
                else
                    options.video = arg;
            } else {
                areArgsOk = false;
            }
        }

        LocalFree(static_cast<HLOCAL>(argv));
        return areArgsOk;
    }

    LRESULT CALLBACK OutputProc(const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam) {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT CALLBACK ControlProc(const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam) {
        if (mainApp.taskbarCreatedMessage != 0 && message == mainApp.taskbarCreatedMessage) {
            mainApp.needRebuild = true;
            return 0;
        }

        switch (message) {
            case WM_DISPLAYCHANGE:
                mainApp.needRebuild = true;
                return 0;
            case WM_SETTINGCHANGE:
                if (wparam == SPI_SETWORKAREA)
                    mainApp.needRebuild = true;
                return 0;
            case WM_CLOSE:
            case WM_ENDSESSION:
            case WM_DESTROY:
                mainApp.running = false;
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool RegisterClasses(const HINSTANCE instance) {

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

        wc.lpfnWndProc = ControlProc;
        wc.lpszClassName = kControlClass;
        if (RegisterClassExW(&wc) == 0)
            return false;

        wc.lpfnWndProc = OutputProc;
        wc.lpszClassName = kOutputClass;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        return RegisterClassExW(&wc) != 0;
    }

    int stopGlassInstance() {
        const HWND existing = FindWindowW(kControlClass, nullptr);
        if (existing != nullptr)
            PostMessageW(existing, WM_CLOSE, 0, 0);
        return 0;
    }

    void ApplyVideoFormat() {
        float offsetU = 0.0f;
        float offsetV = 0.0f;
        float scaleU = 1.0f;
        float scaleV = 1.0f;
        mainApp.video.SourceRect(&offsetU, &offsetV, &scaleU, &scaleV);

        mainApp.renderer.SetVideoSize(mainApp.video.Width(), mainApp.video.Height());
        mainApp.renderer.SetSourceRect(offsetU, offsetV, scaleU, scaleV);
        mainApp.renderer.SetColorSpace(mainApp.video.GetColorSpace());
    }

    void DestroyOutputs() {
        mainApp.renderer.ClearOutputs();
        for (const HWND hwnd: mainApp.outputs) {
            if (IsWindow(hwnd))
                DestroyWindow(hwnd);
        }
        mainApp.outputs.clear();
        mainApp.host = nullptr;
    }

    bool BuildOutputs() {
        DestroyOutputs();

        mainApp.host = AcquireWallpaperHost();
        if (mainApp.host == nullptr)
            return false;

        const std::vector<RECT> monitors = EnumerateMonitors();
        for (const RECT &monitor: monitors) {
            POINT origin = {.x = monitor.left, .y = monitor.top};
            ScreenToClient(mainApp.host, &origin);

            const int width = monitor.right - monitor.left;
            const int height = monitor.bottom - monitor.top;

            HWND output = CreateWindowExW(WS_EX_NOACTIVATE, kOutputClass, L"",
                                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                          origin.x, origin.y, width, height,
                                          mainApp.host, nullptr, mainApp.instance, nullptr);
            if (output == nullptr)
                continue;

            if (!mainApp.renderer.AddOutput(output, static_cast<UINT>(width), static_cast<UINT>(height))) {
                DestroyWindow(output);
                continue;
            }
            mainApp.outputs.push_back(output);
        }

        return !mainApp.outputs.empty();
    }

    bool initGraphics() {
        if (!mainApp.renderer.CreateDevice()) {
            showFatal(L"Failed to start Direct3D 11.");
            return false;
        }
        if (!mainApp.video.Open(mainApp.options.video, mainApp.renderer.Device(), mainApp.renderer.Context())) {
            showFatal(L"Failed to load video file :\n%s\n\nFile can't be found or file format is not supported by Media Foundation.", mainApp.options.video.c_str());
            return false;
        }

        ApplyVideoFormat();

        BuildOutputs();
        return true;
    }

    void shutdownGraphics() {
        DestroyOutputs();
        mainApp.video.Close();
        mainApp.renderer.Shutdown();
    }

    void runLoop() {
        MediaClock clock;
        bool haveFrame = false;
        LONGLONG frameTimestamp = 0;
        bool suspended = false;
        LONGLONG suspendedAt = 0;
        LONGLONG lastHostCheck = 0;
        LONGLONG lastSuspendCheck = 0;
        bool suspendWanted = false;
        unsigned formatVersion = mainApp.video.FormatVersion();

        while (mainApp.running) {
            if (!pumpMessages())
                break;

            if (mainApp.needRebuild) {
                mainApp.needRebuild = false;
                BuildOutputs();
            }

            if (!mainApp.renderer.HasOutputs()) {
                waitOrPumpMessages(1000);
                BuildOutputs();
                continue;
            }

            // Explorer may have been restarted: check once per second that our windows still exist.
            if (const LONGLONG now = qpcNow(); now - lastHostCheck > g_qpcFrequency) {
                lastHostCheck = now;
                if (!IsWindow(mainApp.host) || (!mainApp.outputs.empty() && !IsWindow(mainApp.outputs.front()))) {
                    BuildOutputs();
                    continue;
                }
            }

            if (const LONGLONG now = qpcNow(); suspended || now - lastSuspendCheck > g_qpcFrequency / 4) {
                lastSuspendCheck = now;
                suspendWanted = shouldSuspend();
            }

            if (suspendWanted) {
                if (!suspended) {
                    suspended = true;
                    suspendedAt = qpcNow();
                }
                waitOrPumpMessages(500);
                continue;
            }
            if (suspended) {
                suspended = false;
                clock.base += qpcNow() - suspendedAt;
            }

            if (!haveFrame) {
                bool looped = false;
                if (!mainApp.video.DecodeNext(&frameTimestamp, &looped)) {
                    shutdownGraphics();
                    if (!initGraphics())
                        break;
                    clock.needsRebase = true;
                    formatVersion = mainApp.video.FormatVersion();
                    continue;
                }
                if (looped)
                    clock.needsRebase = true;
                if (clock.needsRebase)
                    clock.rebaseTo(frameTimestamp);
                if (mainApp.video.FormatVersion() != formatVersion) {
                    formatVersion = mainApp.video.FormatVersion();
                    ApplyVideoFormat();
                }
                haveFrame = true;
            }

            const LONGLONG mediaNow = clock.now();
            if (constexpr LONGLONG kPresentSlack = 5000; frameTimestamp <= mediaNow + kPresentSlack) {
                if (!mainApp.renderer.RenderAll(mainApp.video.PlaneY(), mainApp.video.PlaneUV())) {
                    // Device lost: rebuild everything from scratch.
                    shutdownGraphics();
                    if (!initGraphics())
                        break;
                    clock.needsRebase = true;
                    formatVersion = mainApp.video.FormatVersion();
                    haveFrame = false;
                    continue;
                }
                haveFrame = false;
                if (mediaNow - frameTimestamp > kUnitsPerSecond)
                    clock.needsRebase = true;
            } else {
                const LONGLONG waitUnits = frameTimestamp - mediaNow;
                const DWORD waitMs = static_cast<DWORD>((std::min<LONGLONG>)(waitUnits / 10000, 50));
                waitOrPumpMessages(waitMs);
            }
        }
    }
}

int initGleamingGlass(const HINSTANCE hInstance) {
    mainApp.instance = hInstance;

    if (!parseArguments(mainApp.options)) {
        return 1;
    }

    switch (mainApp.options.action) {
        case Action::Stop:
            return stopGlassInstance();
        case Action::Run:
            break;
    }

    if (mainApp.options.video.empty()) {
        return 1;
    }

    constexpr wchar_t kMutexName[] = L"Local\\GleamingGlass.SingleInstance";
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        stopGlassInstance();
        if (WaitForSingleObject(mutex, 5000) == WAIT_TIMEOUT) {
            showFatal(L"Another instance isn't responding.");
            CloseHandle(mutex);
            return 1;
        }
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFrequency = freq.QuadPart;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if (g_timer == nullptr)
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    int exitCode = 1;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            if (RegisterClasses(hInstance)) {
                mainApp.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
                mainApp.control = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kControlClass, L"GleamingGlass", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
                if (mainApp.control != nullptr) {
                    if (initGraphics()) {
                        if (mainApp.renderer.HasOutputs()) {
                            runLoop();
                            exitCode = 0;
                        } else {
                            showFatal(L"Failed to attach to the desktop: the shell doesn't expose "
                                      L"the expected wallpaper window.\n\nMake sure Explorer is running.");
                        }
                    }
                    shutdownGraphics();
                    DestroyWindow(mainApp.control);
                } else {
                    showFatal(L"Failed to create the control window.");
                }
            }
            MFShutdown();
        } else {
            showFatal(L"Media Foundation is unavailable.");
        }
        CoUninitialize();
    }

    if (g_timer != nullptr)
        CloseHandle(g_timer);
    if (mutex != nullptr) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return exitCode;
}
