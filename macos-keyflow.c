/*
 * MacKeyFlow.exe
 * Lightweight Win32 toolkit. Features:
 *   1. CapsRemap  — CapsLock -> Ctrl+Space
 *   2. EscClose   — Hold ESC 1.0s -> Alt+F4, with on-screen countdown overlay
 *                   (200ms activation delay; short taps pass through unchanged)
 *   3. WinBacktick — Win+` cycles windows of the same app (like macOS Cmd+`)
 *
 * Build (MSVC):    build.bat
 * Build (MinGW):   windres macos-keyflow.rc -o macos-keyflow.o
 *                  gcc -O2 -s -mwindows macos-keyflow.c macos-keyflow.o -o macos-keyflow.exe
 *                       -luser32 -lshell32 -ladvapi32 -lgdi32 -ldwmapi -lpsapi
 */

#ifdef _MSC_VER
#   pragma comment(lib, "user32.lib")
#   pragma comment(lib, "shell32.lib")
#   pragma comment(lib, "advapi32.lib")
#   pragma comment(lib, "gdi32.lib")
#   pragma comment(lib, "dwmapi.lib")
#   pragma comment(lib, "psapi.lib")
#   pragma comment(lib, "ole32.lib")
#   pragma comment(lib, "oleaut32.lib")
#   pragma comment(lib, "taskschd.lib")
#   ifndef KEYFLOW_TESTING
#       pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#   endif
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include <limits.h>
#include <sddl.h>
#include <taskschd.h>

/* ── Message / timer IDs ── */
#define IDI_APP              1

#define WM_TRAY_MSG          (WM_APP + 1)
#define WM_ESC_DOWN          (WM_APP + 2)
#define WM_ESC_UP            (WM_APP + 3)
#define WM_CAPS_DOWN         (WM_APP + 4)
#define WM_CAPS_UP           (WM_APP + 5)
#define WM_APP_EXIT          (WM_APP + 6)

#define TIMER_ESC_DELAY      1    /* 200ms one-shot: ignores short taps        */
#define TIMER_ESC_TICK       2    /* 30ms recurring: drives progress animation */
#define TIMER_CAPS           3    /* one-shot: CapsLock long-press threshold   */
#define TIMER_HOTKEY_RETRY   4    /* retry Win+` registration after a conflict */
#define TIMER_TRAY_RETRY     5
#define TIMER_HEALTH         6

#define HOTKEY_WIN_BACKTICK  1    /* RegisterHotKey ID for Win+`               */

#define ESC_DELAY_MS         200  /* ms before overlay activates               */
#define ESC_HOLD_MS          1000 /* ms of hold required to fire Alt+F4        */
#define ESC_TICK_MS          30   /* repaint interval (ms)                     */
#define CAPS_HOLD_MS         400  /* ms to distinguish tap vs. long-press      */
#define HOTKEY_RETRY_MS      5000 /* retry interval when Win+` is unavailable   */

/* ── Overlay geometry ── */
#define OV_W                 260
#define OV_H                 44
#define OV_PAD               14   /* horizontal margin */
#define OV_BAR_Y             32   /* bar top y         */
#define OV_BAR_H             6    /* bar height        */

/* ── Tray menu IDs ── */
#define ID_MENU_STARTUP      100
#define ID_MENU_EXIT         101
#define ID_MENU_ABOUT        102
#define ID_MENU_LOG          103
#define ID_MENU_REPAIR       104

#define PRODUCT_NAME         L"MacKeyFlow"
#define TASK_NAME            L"MacKeyFlow"
#define LEGACY_TASK_NAME     L"MyToolKit"
#define MUTEX_NAME           L"Global\\MacKeyFlow_SingleInstance"
#define LEGACY_MUTEX_NAME    L"Global\\MyToolKit_SingleInstance"
#define WND_CLASS_MAIN       L"MacKeyFlow_TrayWnd"
#define WND_CLASS_OVERLAY    L"MacKeyFlow_OverlayWnd"
#define ABOUT_URL            L"https://github.com/qcbf/macos-keyflow"
#define LOG_FILE_NAME        L"macos-keyflow.log"
#define CRASH_LOG_FILE_NAME  L"macos-keyflow-crash.log"

/* ── Globals ── */
static HHOOK           s_kbHook;
static HANDLE          s_hKeyboardThread;
static DWORD           s_keyboardThreadId;
static HANDLE          s_hKeyboardReady;
static HANDLE          s_hKeyboardStop;
static HINSTANCE       s_hInst;
static NOTIFYICONDATAW s_nid;
static HWND            s_hwndMain;
static HWND            s_hwndOverlay;
static HANDLE          s_hMutex;
static volatile LONG   s_escDown;      /* ESC physically held      */
static int             s_escElapsedMs; /* ms elapsed in countdown  */
static volatile LONG   s_capsDown;     /* CapsLock physically held */
static BOOL            s_capsPending;
static BOOL            s_capsLongFired;
static DWORD           s_capsPressedAt;
static BOOL            s_escTracking;
static DWORD           s_escPressedAt;
static HWND            s_escTarget;
static BOOL            s_hotkeyRegistered;
static BOOL            s_trayIconAdded;
static BOOL            s_shutdownRequested;
static volatile LONG   s_hookPostError;
static UINT            s_wmTaskbarCreated; /* "TaskbarCreated" msg ID */


/* ════════════════════════════════════════════════════════════════
 *  Diagnostics
 *
 *  The program is deliberately silent while it is working.  A small
 *  append-only log makes a clean exit, initialization failure, or native
 *  exception distinguishable from a tray icon that merely disappeared.
 * ════════════════════════════════════════════════════════════════ */
static BOOL BuildLogPath(LPCWSTR fileName, WCHAR* path, DWORD cchPath)
{
    DWORD length;

    if (!fileName || !path || cchPath == 0) return FALSE;
    path[0] = L'\0';
    length = GetTempPathW(cchPath, path);
    if (length == 0 || length >= cchPath) return FALSE;
    return wcscat_s(path, cchPath, fileName) == 0;
}

static void AppendLogLine(LPCWSTR line)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    SYSTEMTIME now;
    WCHAR buffer[1024];
    LARGE_INTEGER fileSize;
    BYTE bom[2] = { 0xFF, 0xFE };
    DWORD bytes;
    int length;

    if (!BuildLogPath(LOG_FILE_NAME, path, MAX_PATH)) return;
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    fileSize.QuadPart = 0;
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == 0)
        WriteFile(file, bom, sizeof(bom), &bytes, NULL);

    GetLocalTime(&now);
    length = _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                          L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
                          now.wYear, now.wMonth, now.wDay,
                          now.wHour, now.wMinute, now.wSecond,
                          line ? line : L"(null)");
    if (length > 0)
        WriteFile(file, buffer, (DWORD)(length * sizeof(WCHAR)), &bytes, NULL);
    CloseHandle(file);
}

static void LogWin32Error(LPCWSTR operation, DWORD error)
{
    WCHAR line[512];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%s failed (error=%lu)", operation, error);
    AppendLogLine(line);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    SYSTEMTIME now;
    WCHAR buffer[512];
    LARGE_INTEGER fileSize;
    BYTE bom[2] = { 0xFF, 0xFE };
    DWORD bytes;
    DWORD code = 0;
    PVOID address = NULL;
    int length;

    if (exception && exception->ExceptionRecord)
    {
        code = exception->ExceptionRecord->ExceptionCode;
        address = exception->ExceptionRecord->ExceptionAddress;
    }
    if (BuildLogPath(CRASH_LOG_FILE_NAME, path, MAX_PATH))
    {
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            fileSize.QuadPart = 0;
            if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == 0)
                WriteFile(file, bom, sizeof(bom), &bytes, NULL);
            GetLocalTime(&now);
            length = _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                                  L"[%04u-%02u-%02u %02u:%02u:%02u] exception=0x%08lX address=%p\r\n",
                                  now.wYear, now.wMonth, now.wDay,
                                  now.wHour, now.wMinute, now.wSecond,
                                  code, address);
            if (length > 0)
                WriteFile(file, buffer, (DWORD)(length * sizeof(WCHAR)), &bytes, NULL);
            CloseHandle(file);
        }
    }
    /* Preserve Windows Error Reporting as well as our own exception record. */
    return EXCEPTION_CONTINUE_SEARCH;
}

static void PostMainMessage(UINT message, DWORD time, LPARAM data)
{
    if (PostMessageW(s_hwndMain, message, (WPARAM)time, data)) return;
    /* The UI thread logs failures later; the hook must never wait on file I/O. */
    InterlockedExchange(&s_hookPostError, (LONG)GetLastError());
}

static BOOL SendKeyboardInputs(INPUT* inputs, UINT count, LPCWSTR operation)
{
    UINT sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent == count) return TRUE;
    LogWin32Error(operation, GetLastError());
    return FALSE;
}

static LRESULT CALLBACK OnKeyboardEvent(int code, WPARAM wParam, LPARAM lParam);

static DWORD WINAPI KeyboardThreadProc(LPVOID parameter)
{
    MSG msg;
    DWORD result;
    BOOL running = TRUE;
    (void)parameter;

    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, OnKeyboardEvent, s_hInst, 0);
    if (!s_kbHook)
        LogWin32Error(L"SetWindowsHookExW(WH_KEYBOARD_LL)", GetLastError());
    SetEvent(s_hKeyboardReady);
    if (!s_kbHook) return 1;

    while (running)
    {
        result = MsgWaitForMultipleObjectsEx(1, &s_hKeyboardStop, INFINITE,
                                             QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED)
        {
            LogWin32Error(L"MsgWaitForMultipleObjectsEx(keyboard)", GetLastError());
            break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = FALSE; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    UnhookWindowsHookEx(s_kbHook);
    return 0;
}

static BOOL StopKeyboardHook(void)
{
    if (s_hKeyboardThread)
    {
        SetEvent(s_hKeyboardStop);
        if (WaitForSingleObject(s_hKeyboardThread, 5000) != WAIT_OBJECT_0)
        {
            AppendLogLine(L"Keyboard thread did not stop within five seconds.");
            /* Keep its handles alive until process exit; never free live state. */
            return FALSE;
        }
        CloseHandle(s_hKeyboardThread);
        s_hKeyboardThread = NULL;
    }
    if (s_hKeyboardReady) CloseHandle(s_hKeyboardReady);
    if (s_hKeyboardStop) CloseHandle(s_hKeyboardStop);
    s_hKeyboardReady = NULL;
    s_hKeyboardStop = NULL;
    s_kbHook = NULL;
    s_keyboardThreadId = 0;
    return TRUE;
}

static BOOL StartKeyboardHook(void)
{
    DWORD waitResult;
    if (!StopKeyboardHook()) return FALSE;
    InterlockedExchange(&s_capsDown, FALSE);
    InterlockedExchange(&s_escDown, FALSE);
    s_hKeyboardReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_hKeyboardStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_hKeyboardReady || !s_hKeyboardStop)
    {
        LogWin32Error(L"CreateEventW(keyboard)", GetLastError());
        StopKeyboardHook();
        return FALSE;
    }
    s_hKeyboardThread = CreateThread(NULL, 0, KeyboardThreadProc, NULL, 0,
                                     &s_keyboardThreadId);
    if (!s_hKeyboardThread)
    {
        LogWin32Error(L"CreateThread(keyboard)", GetLastError());
        StopKeyboardHook();
        return FALSE;
    }
    waitResult = WaitForSingleObject(s_hKeyboardReady, 5000);
    if (waitResult != WAIT_OBJECT_0 || !s_kbHook)
    {
        AppendLogLine(L"Keyboard hook startup failed.");
        StopKeyboardHook();
        return FALSE;
    }
    AppendLogLine(L"Keyboard hook ready on its dedicated thread.");
    return TRUE;
}


/* ════════════════════════════════════════════════════════════════
 *  Feature 1: CapsRemap  (CapsLock -> Ctrl+Space)
 *  Feature 2: EscClose   (ESC held -> Alt+F4)
 * ════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK OnKeyboardEvent(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;

        if (!(kb->flags & LLKHF_INJECTED))
        {
            /* ── CapsRemap / CapsToggle ──
             *   Tap  (<400ms): Ctrl+Space  (input method switch)
             *   Hold (>=400ms): real CapsLock toggle (macOS style)
             *   All non-injected CapsLock events are suppressed here;
             *   the actual actions are fired from WndProc timers.     */
            if (kb->vkCode == VK_CAPITAL)
            {
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                    InterlockedCompareExchange(&s_capsDown, 0, 0) == 0)
                {
                    InterlockedExchange(&s_capsDown, TRUE);
                    PostMainMessage(WM_CAPS_DOWN, kb->time, 0);
                }
                else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
                         InterlockedCompareExchange(&s_capsDown, 0, 0) != 0)
                {
                    InterlockedExchange(&s_capsDown, FALSE);
                    PostMainMessage(WM_CAPS_UP, kb->time, 0);
                }
                return 1; /* always suppress physical CapsLock */
            }

            /* ── EscClose: track first down / up only ── */
            if (kb->vkCode == VK_ESCAPE)
            {
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                    InterlockedCompareExchange(&s_escDown, 0, 0) == 0)
                {
                    InterlockedExchange(&s_escDown, TRUE);
                    PostMainMessage(WM_ESC_DOWN, kb->time, (LPARAM)GetForegroundWindow());
                }
                else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
                         InterlockedCompareExchange(&s_escDown, 0, 0) != 0)
                {
                    InterlockedExchange(&s_escDown, FALSE);
                    PostMainMessage(WM_ESC_UP, kb->time, 0);
                }
                /* ESC always passes through so apps receive it normally. */
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}


/* ════════════════════════════════════════════════════════════════
 *  ESC countdown overlay (subtle, low-profile)
 * ════════════════════════════════════════════════════════════════ */
static BYTE LerpByte(BYTE a, BYTE b, float t)
{
    return (BYTE)(a + (int)((b - a) * t));
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Double-buffer */
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, OV_W, OV_H);
        if (!memDC || !memBmp)
        {
            if (memBmp) DeleteObject(memBmp);
            if (memDC) DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        /* Background */
        RECT rc = { 0, 0, OV_W, OV_H };
        HBRUSH bgBrush = CreateSolidBrush(RGB(36, 36, 36));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        /* Bar track */
        RECT trackRc = { OV_PAD, OV_BAR_Y, OV_W - OV_PAD, OV_BAR_Y + OV_BAR_H };
        HBRUSH trackBrush = CreateSolidBrush(RGB(62, 62, 62));
        FillRect(memDC, &trackRc, trackBrush);
        DeleteObject(trackBrush);

        /* Bar fill — muted teal -> amber */
        float progress = (float)s_escElapsedMs / (float)ESC_HOLD_MS;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        int fillW = (int)((OV_W - OV_PAD * 2) * progress);
        if (fillW > 0)
        {
            BYTE r = LerpByte(70,  200, progress);
            BYTE g = LerpByte(150, 70,  progress);
            BYTE b = LerpByte(120, 50,  progress);
            HBRUSH fillBrush = CreateSolidBrush(RGB(r, g, b));
            RECT fillRc = { OV_PAD, OV_BAR_Y, OV_PAD + fillW, OV_BAR_Y + OV_BAR_H };
            FillRect(memDC, &fillRc, fillBrush);
            DeleteObject(fillBrush);
        }

        /* Text */
        int remainSec = (ESC_HOLD_MS - s_escElapsedMs + 999) / 1000;
        if (remainSec < 1) remainSec = 1;
        WCHAR text[32];
        wsprintfW(text, L"Hold ESC · %ds", remainSec);

        HFONT hFont = CreateFontW(
            14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldFont = NULL;

        if (hFont)
            oldFont = (HFONT)SelectObject(memDC, hFont);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(170, 170, 170));
        RECT textRc = { OV_PAD, 8, OV_W - OV_PAD, OV_BAR_Y - 2 };
        DrawTextW(memDC, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (hFont)
        {
            SelectObject(memDC, oldFont);
            DeleteObject(hFont);
        }

        BitBlt(hdc, 0, 0, OV_W, OV_H, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void HideEscOverlay(void);

static void ShowEscOverlay(void)
{
    /* Center on the monitor that contains the current foreground window. */
    HWND hFg = GetForegroundWindow();
    HMONITOR hMon = MonitorFromWindow(hFg ? hFg : s_hwndMain, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) return;
    int x = mi.rcMonitor.left + (mi.rcMonitor.right  - mi.rcMonitor.left - OV_W) / 2;
    int y = mi.rcMonitor.top  + (mi.rcMonitor.bottom - mi.rcMonitor.top  - OV_H) / 2;

    if (s_hwndOverlay) HideEscOverlay();

    s_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WND_CLASS_OVERLAY, NULL, WS_POPUP,
        x, y, OV_W, OV_H,
        NULL, NULL, s_hInst, NULL);

    if (s_hwndOverlay)
    {
        SetLayeredWindowAttributes(s_hwndOverlay, 0, 148, LWA_ALPHA); /* ~58% opacity */
        ShowWindow(s_hwndOverlay, SW_SHOWNOACTIVATE);
        UpdateWindow(s_hwndOverlay);
    }
    else
        LogWin32Error(L"CreateWindowExW(overlay)", GetLastError());
}

static void HideEscOverlay(void)
{
    if (s_hwndOverlay)
    {
        DestroyWindow(s_hwndOverlay);
        s_hwndOverlay = NULL;
    }
}


/* ════════════════════════════════════════════════════════════════
 *  Elevation helpers
 * ════════════════════════════════════════════════════════════════ */
static BOOL IsRunningElevated(void)
{
    BOOL elevated = FALSE;
    HANDLE token  = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION elev;
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, size, &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated;
}

static BOOL EnsureElevated(void)
{
    WCHAR exePath[MAX_PATH] = { 0 };
    SHELLEXECUTEINFOW sei;
    DWORD length;

    if (IsRunningElevated()) return TRUE;

    length = GetModuleFileNameW(NULL, exePath, _countof(exePath));
    if (length == 0 || length >= _countof(exePath))
    {
        LogWin32Error(L"GetModuleFileNameW", GetLastError());
        MessageBoxW(NULL, L"无法确定程序路径，无法请求管理员权限。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return FALSE;
    }

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow  = SW_HIDE;
    if (!ShellExecuteExW(&sei))
    {
        DWORD error = GetLastError();
        LogWin32Error(L"ShellExecuteExW(runas)", error);
        if (error != ERROR_CANCELLED)
            MessageBoxW(NULL, L"无法以管理员身份启动程序。",
                        PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return FALSE;
    }

    /* The elevated child owns the real application lifetime. */
    ExitProcess(0);
}


/* ════════════════════════════════════════════════════════════════
 *  Single-instance guard
 * ════════════════════════════════════════════════════════════════ */
static BOOL AcquireSingleInstance(void)
{
    HANDLE legacyMutex;

    SetLastError(ERROR_SUCCESS);
    s_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (!s_hMutex)
    {
        LogWin32Error(L"CreateMutexW", GetLastError());
        MessageBoxW(NULL, L"无法创建单实例锁，程序不会启动。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(NULL,
            L"MacKeyFlow 已在运行中。\n\n"
            L"请勿重复启动，程序会在后台持续工作。",
            PRODUCT_NAME, MB_OK | MB_ICONWARNING);
        return FALSE;
    }

    /* Detect a still-running binary from the previous project name. */
    legacyMutex = OpenMutexW(SYNCHRONIZE, FALSE, LEGACY_MUTEX_NAME);
    if (legacyMutex)
    {
        CloseHandle(legacyMutex);
        MessageBoxW(NULL,
            L"旧版 MyToolKit 仍在运行。\n\n"
            L"请先退出旧版程序，再启动 MacKeyFlow。",
            PRODUCT_NAME, MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    return TRUE;
}


/* ════════════════════════════════════════════════════════════════
 *  Auto-start (Task Scheduler)
 * ════════════════════════════════════════════════════════════════ */
typedef struct {
    ITaskService* service;
    ITaskFolder* root;
    BOOL uninitialize;
} Scheduler;

static void CloseScheduler(Scheduler* scheduler)
{
    if (scheduler->root) ITaskFolder_Release(scheduler->root);
    if (scheduler->service) ITaskService_Release(scheduler->service);
    if (scheduler->uninitialize) CoUninitialize();
    ZeroMemory(scheduler, sizeof(*scheduler));
}

static HRESULT OpenScheduler(Scheduler* scheduler)
{
    HRESULT hr;
    VARIANT empty;
    BSTR rootName;

    ZeroMemory(scheduler, sizeof(*scheduler));
    VariantInit(&empty);
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
    scheduler->uninitialize = SUCCEEDED(hr);
    hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ITaskService, (void**)&scheduler->service);
    if (SUCCEEDED(hr))
        hr = ITaskService_Connect(scheduler->service, empty, empty, empty, empty);
    if (SUCCEEDED(hr))
    {
        rootName = SysAllocString(L"\\");
        hr = rootName ? ITaskService_GetFolder(scheduler->service, rootName,
                                              &scheduler->root) : E_OUTOFMEMORY;
        SysFreeString(rootName);
    }
    if (FAILED(hr)) CloseScheduler(scheduler);
    return hr;
}

static BOOL IsTaskPresent(LPCWSTR taskName)
{
    Scheduler scheduler;
    IRegisteredTask* task = NULL;
    BSTR name;
    HRESULT hr = OpenScheduler(&scheduler);
    if (FAILED(hr)) return FALSE;
    name = SysAllocString(taskName);
    hr = name ? ITaskFolder_GetTask(scheduler.root, name, &task) : E_OUTOFMEMORY;
    SysFreeString(name);
    if (task) IRegisteredTask_Release(task);
    CloseScheduler(&scheduler);
    return SUCCEEDED(hr);
}

static BOOL IsStartupEnabled(void)
{
    return IsTaskPresent(TASK_NAME);
}

static BOOL EscapeXml(LPCWSTR source, WCHAR* destination, size_t capacity)
{
    size_t used = 0;
    while (*source)
    {
        LPCWSTR escaped = NULL;
        size_t length;
        switch (*source)
        {
        case L'&': escaped = L"&amp;"; break;
        case L'<': escaped = L"&lt;"; break;
        case L'>': escaped = L"&gt;"; break;
        case L'\"': escaped = L"&quot;"; break;
        case L'\'': escaped = L"&apos;"; break;
        }
        length = escaped ? wcslen(escaped) : 1;
        if (used + length >= capacity) return FALSE;
        if (escaped) memcpy(destination + used, escaped, length * sizeof(WCHAR));
        else destination[used] = *source;
        used += length;
        ++source;
    }
    if (used >= capacity) return FALSE;
    destination[used] = L'\0';
    return TRUE;
}

/* PT0S is essential: schtasks' default PT72H kills resident applications. */
static BOOL BuildStartupXml(LPCWSTR exePath, LPCWSTR userSid,
                            WCHAR* xml, size_t capacity)
{
    WCHAR escapedPath[MAX_PATH * 6];
    WCHAR escapedSid[256];
    if (!EscapeXml(exePath, escapedPath, _countof(escapedPath)) ||
        !EscapeXml(userSid, escapedSid, _countof(escapedSid))) return FALSE;
    return _snwprintf_s(xml, capacity, _TRUNCATE,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId>"
        L"</LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"User\"><UserId>%s</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>"
        L"</Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<StartWhenAvailable>true</StartWhenAvailable>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
        L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>"
        L"</Settings><Actions Context=\"User\"><Exec><Command>%s</Command></Exec>"
        L"</Actions></Task>", escapedSid, escapedSid, escapedPath) >= 0;
}

static HRESULT ChangeStartupTask(LPCWSTR taskName, BOOL enable)
{
    Scheduler scheduler;
    IRegisteredTask* task = NULL;
    HANDLE token = NULL;
    TOKEN_USER* user = NULL;
    LPWSTR sid = NULL;
    BSTR name = NULL;
    BSTR definition = NULL;
    VARIANT empty;
    WCHAR exePath[MAX_PATH];
    WCHAR xml[4096];
    DWORD length;
    DWORD userSize = 0;
    HRESULT hr = OpenScheduler(&scheduler);

    if (FAILED(hr)) return hr;
    VariantInit(&empty);
    name = SysAllocString(taskName);
    if (!name) { hr = E_OUTOFMEMORY; goto cleanup; }
    if (!enable)
    {
        hr = ITaskFolder_DeleteTask(scheduler.root, name, 0);
        goto cleanup;
    }
    length = GetModuleFileNameW(NULL, exePath, _countof(exePath));
    if (!length || length >= _countof(exePath))
    {
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        goto cleanup;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &userSize);
    user = (TOKEN_USER*)malloc(userSize);
    if (!user) { hr = E_OUTOFMEMORY; goto cleanup; }
    if (!GetTokenInformation(token, TokenUser, user, userSize, &userSize) ||
        !ConvertSidToStringSidW(user->User.Sid, &sid))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    if (!BuildStartupXml(exePath, sid, xml, _countof(xml)))
    {
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        goto cleanup;
    }
    definition = SysAllocString(xml);
    hr = definition ? ITaskFolder_RegisterTask(
        scheduler.root, name, definition, TASK_CREATE_OR_UPDATE,
        empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &task) : E_OUTOFMEMORY;
cleanup:
    if (task) IRegisteredTask_Release(task);
    SysFreeString(definition);
    SysFreeString(name);
    if (sid) LocalFree(sid);
    free(user);
    if (token) CloseHandle(token);
    CloseScheduler(&scheduler);
    if (FAILED(hr)) LogWin32Error(L"Task Scheduler", (DWORD)hr);
    return hr;
}

static void MigrateLegacyStartup(void)
{
    BOOL legacyPresent = IsTaskPresent(LEGACY_TASK_NAME);
    HRESULT hr;
    if (!legacyPresent && !IsStartupEnabled()) return;

    /* Create/update first. Never remove the old task if registration fails. */
    hr = ChangeStartupTask(TASK_NAME, TRUE);
    if (SUCCEEDED(hr) && legacyPresent)
        hr = ChangeStartupTask(LEGACY_TASK_NAME, FALSE);
    if (SUCCEEDED(hr))
        AppendLogLine(L"Startup task updated: unlimited duration, battery operation enabled.");
    else
        MessageBoxW(NULL, L"更新开机启动失败。请通过托盘菜单重新设置，并查看日志。",
                    PRODUCT_NAME, MB_OK | MB_ICONWARNING);
}

static void ToggleStartup(void)
{
    BOOL enable = !IsStartupEnabled();
    if (FAILED(ChangeStartupTask(TASK_NAME, enable)))
        MessageBoxW(NULL, L"修改开机启动失败。请确保以管理员权限运行，并查看日志。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
}


/* ════════════════════════════════════════════════════════════════
 *  Feature 3: WinBacktick — Win+` cycles windows of the same app.
 *
 *  Design rule: RegisterHotKey is the only owner of Win+`. The low-level
 *  keyboard hook must not suppress any part of this chord. Suppressing ` or Win
 *  forces us to fake key-up/key-down state with SendInput, which races with the
 *  shell and causes Start menu popups, stuck logical Win, or missed switches.
 *
 *  "Same app" is matched by executable path instead of PID so multi-process
 *  apps such as Chrome, VS Code, Windows Terminal, and Electron apps still cycle
 *  together.
 * ════════════════════════════════════════════════════════════════ */
typedef struct {
    WCHAR  exePath[MAX_PATH];
    HWND*  list;
    int    count;
    int    capacity;
    BOOL   failed;
} EnumSameAppCtx;

static HWND GetRootAppWindow(HWND hwnd)
{
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    return root ? root : hwnd;
}

static BOOL GetWindowExePath(HWND hwnd, WCHAR* exePath, DWORD cchExePath)
{
    DWORD pid = 0;
    HANDLE process;
    DWORD size;

    if (!exePath || cchExePath < 2) return FALSE;
    exePath[0] = L'\0';
    exePath[cchExePath - 1] = L'\0';
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process)
    {
        size = cchExePath - 1;
        if (QueryFullProcessImageNameW(process, 0, exePath, &size))
        {
            exePath[size] = L'\0';
            CloseHandle(process);
            return TRUE;
        }
        CloseHandle(process);
    }

    process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return FALSE;
    if (!GetModuleFileNameExW(process, NULL, exePath, cchExePath))
    {
        CloseHandle(process);
        exePath[0] = L'\0';
        return FALSE;
    }
    CloseHandle(process);
    return TRUE;
}

static BOOL IsSwitchableWindow(HWND hwnd)
{
    LONG style;
    LONG exStyle;
    HWND owner;
    BOOL cloaked = FALSE;

    if (!IsWindowVisible(hwnd)) return FALSE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;

    style = GetWindowLongW(hwnd, GWL_STYLE);
    exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) return FALSE;
    if (exStyle & WS_EX_TOOLWINDOW) return FALSE;
    if (GetWindowTextLengthW(hwnd) == 0) return FALSE;

    owner = GetWindow(hwnd, GW_OWNER);
    if (owner && !(exStyle & WS_EX_APPWINDOW)) return FALSE;

    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return FALSE;

    return TRUE;
}

static BOOL CALLBACK EnumSameAppProc(HWND hwnd, LPARAM lParam)
{
    EnumSameAppCtx* ctx = (EnumSameAppCtx*)lParam;
    WCHAR exePath[MAX_PATH];

    if (ctx->failed) return FALSE;
    if (!IsSwitchableWindow(hwnd)) return TRUE;
    if (!GetWindowExePath(hwnd, exePath, MAX_PATH)) return TRUE;
    if (lstrcmpiW(exePath, ctx->exePath) != 0) return TRUE;

    if (ctx->count >= ctx->capacity)
    {
        int newCapacity;
        if (ctx->capacity > INT_MAX / 2)
        {
            ctx->failed = TRUE;
            return FALSE;
        }
        newCapacity = ctx->capacity ? ctx->capacity * 2 : 16;
        if ((size_t)newCapacity > SIZE_MAX / sizeof(HWND))
        {
            ctx->failed = TRUE;
            return FALSE;
        }
        HWND* newList = (HWND*)realloc(ctx->list, newCapacity * sizeof(HWND));
        if (!newList)
        {
            ctx->failed = TRUE;
            return FALSE;
        }
        ctx->list = newList;
        ctx->capacity = newCapacity;
    }
    ctx->list[ctx->count++] = hwnd;
    return TRUE;
}

static void ActivateWindow(HWND hwnd)
{
    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
    BOOL attachedForeground = FALSE;
    BOOL attachedTarget = FALSE;

    if (!IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);

    if (foregroundThread && foregroundThread != currentThread)
        attachedForeground = AttachThreadInput(currentThread, foregroundThread, TRUE);
    if (targetThread && targetThread != currentThread && targetThread != foregroundThread)
        attachedTarget = AttachThreadInput(currentThread, targetThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (attachedTarget) AttachThreadInput(currentThread, targetThread, FALSE);
    if (attachedForeground) AttachThreadInput(currentThread, foregroundThread, FALSE);
}

static int CompareWindowHandles(const void* first, const void* second)
{
    UINT_PTR a = (UINT_PTR)*(const HWND*)first;
    UINT_PTR b = (UINT_PTR)*(const HWND*)second;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void SwitchToNextAppWindow(void)
{
    HWND fgWnd = GetRootAppWindow(GetForegroundWindow());
    WCHAR exePath[MAX_PATH];
    EnumSameAppCtx ctx;
    int idx = -1;
    HWND target;

    if (!fgWnd) return;
    if (!GetWindowExePath(fgWnd, exePath, MAX_PATH)) return;

    ZeroMemory(&ctx, sizeof(ctx));
    if (wcscpy_s(ctx.exePath, _countof(ctx.exePath), exePath) != 0) return;
    EnumWindows(EnumSameAppProc, (LPARAM)&ctx);

    if (ctx.failed || ctx.count < 2) { free(ctx.list); return; }

    /* EnumWindows uses Z order, which changes after activation and otherwise
     * makes three or more windows alternate between only the front two. */
    qsort(ctx.list, (size_t)ctx.count, sizeof(HWND), CompareWindowHandles);

    for (int i = 0; i < ctx.count; i++)
    {
        if (ctx.list[i] == fgWnd) { idx = i; break; }
    }

    if (idx < 0) { free(ctx.list); return; }

    target = ctx.list[(idx + 1) % ctx.count];
    ActivateWindow(target);

    free(ctx.list);
}

static void SendShortcut(WORD key, WORD leftModifier, WORD rightModifier,
                         LPCWSTR operation)
{
    INPUT sequence[4];
    UINT count = 0;
    BOOL modifierHeld = (GetAsyncKeyState(leftModifier) & 0x8000) ||
                        (GetAsyncKeyState(rightModifier) & 0x8000);
    ZeroMemory(sequence, sizeof(sequence));
    if (!modifierHeld)
    {
        sequence[count].type = INPUT_KEYBOARD;
        sequence[count++].ki.wVk = leftModifier;
    }
    sequence[count].type = INPUT_KEYBOARD;
    sequence[count++].ki.wVk = key;
    sequence[count].type = INPUT_KEYBOARD;
    sequence[count].ki.wVk = key;
    sequence[count++].ki.dwFlags = KEYEVENTF_KEYUP;
    if (!modifierHeld)
    {
        sequence[count].type = INPUT_KEYBOARD;
        sequence[count].ki.wVk = leftModifier;
        sequence[count++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendKeyboardInputs(sequence, count, operation);
}

static void ToggleCapsLock(void)
{
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = VK_CAPITAL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendKeyboardInputs(inputs, _countof(inputs), L"SendInput(CapsLock)");
}

static void CancelEscHold(HWND hwnd)
{
    KillTimer(hwnd, TIMER_ESC_DELAY);
    KillTimer(hwnd, TIMER_ESC_TICK);
    s_escTracking = FALSE;
    HideEscOverlay();
}

static BOOL IsEscTargetCurrent(void)
{
    DWORD pid = 0;
    if (!s_escTarget || !IsWindow(s_escTarget) ||
        GetForegroundWindow() != s_escTarget) return FALSE;
    GetWindowThreadProcessId(s_escTarget, &pid);
    return pid != 0 && pid != GetCurrentProcessId();
}

static void TickEscHold(HWND hwnd)
{
    DWORD elapsed;
    if (!s_escTracking || !InterlockedCompareExchange(&s_escDown, 0, 0) ||
        !IsEscTargetCurrent())
    {
        CancelEscHold(hwnd);
        return;
    }
    elapsed = (DWORD)GetTickCount64() - s_escPressedAt;
    s_escElapsedMs = (int)(elapsed < ESC_HOLD_MS ? elapsed : ESC_HOLD_MS);
    if (elapsed >= ESC_HOLD_MS)
    {
        CancelEscHold(hwnd);
        /* Keep physical ESC down until its real key-up: auto-repeat must not
         * start another countdown and close the following window as well. */
        SendShortcut(VK_F4, VK_LMENU, VK_RMENU, L"SendInput(Alt+F4)");
    }
    else if (s_hwndOverlay)
        InvalidateRect(s_hwndOverlay, NULL, FALSE);
}


static BOOL RegisterWinBacktickHotkey(HWND hwnd)
{
    static DWORD lastError;
    DWORD error;

    if (!hwnd) return FALSE;
    if (s_hotkeyRegistered) return TRUE;
    if (RegisterHotKey(hwnd, HOTKEY_WIN_BACKTICK,
                       MOD_WIN | MOD_NOREPEAT, VK_OEM_3))
    {
        s_hotkeyRegistered = TRUE;
        lastError = 0;
        KillTimer(hwnd, TIMER_HOTKEY_RETRY);
        AppendLogLine(L"Win+` hotkey registered.");
        return TRUE;
    }

    error = GetLastError();
    s_hotkeyRegistered = FALSE;
    if (error != lastError) LogWin32Error(L"RegisterHotKey(Win+`)", error);
    lastError = error;
    if (!SetTimer(hwnd, TIMER_HOTKEY_RETRY, HOTKEY_RETRY_MS, NULL))
        LogWin32Error(L"SetTimer(hotkey retry)", GetLastError());
    return FALSE;
}


/* ════════════════════════════════════════════════════════════════
 *  Tray icon & context menu
 * ════════════════════════════════════════════════════════════════ */
static void AddTrayIcon(HWND hwnd)
{
    s_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &s_nid);
    if (s_trayIconAdded)
    {
        s_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
        KillTimer(hwnd, TIMER_TRAY_RETRY);
    }
    else
        SetTimer(hwnd, TIMER_TRAY_RETRY, HOTKEY_RETRY_MS, NULL);
}

static void OpenLogFile(void)
{
    WCHAR path[MAX_PATH];
    if (!BuildLogPath(LOG_FILE_NAME, path, _countof(path))) return;
    if ((INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32)
        LogWin32Error(L"ShellExecuteW(open log)", GetLastError());
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    POINT pt;

    if (!hMenu) {
        LogWin32Error(L"CreatePopupMenu", GetLastError());
        return;
    }
    AppendMenuW(hMenu,
                MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_MENU_STARTUP, L"开机启动");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_REPAIR, L"重新安装键盘钩子");
    AppendMenuW(hMenu, MF_STRING, ID_MENU_LOG, L"打开日志");
    AppendMenuW(hMenu, MF_STRING, ID_MENU_ABOUT, L"关于 MacKeyFlow");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"退出");

    if (!GetCursorPos(&pt))
    {
        LogWin32Error(L"GetCursorPos", GetLastError());
        DestroyMenu(hMenu);
        return;
    }
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(hMenu,
                     TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                     pt.x, pt.y, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    /* ── Tray ── */
    case WM_TRAY_MSG:
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_MENU_STARTUP) ToggleStartup();
        else if (LOWORD(wParam) == ID_MENU_LOG) OpenLogFile();
        else if (LOWORD(wParam) == ID_MENU_REPAIR)
        {
            CancelEscHold(hwnd);
            KillTimer(hwnd, TIMER_CAPS);
            s_capsPending = FALSE;
            if (StopKeyboardHook())
            {
                InterlockedExchange(&s_capsDown, FALSE);
                InterlockedExchange(&s_escDown, FALSE);
                if (!StartKeyboardHook())
                    MessageBoxW(hwnd, L"重新安装键盘钩子失败，请查看日志。",
                                PRODUCT_NAME, MB_OK | MB_ICONERROR);
            }
        }
        else if (LOWORD(wParam) == ID_MENU_ABOUT)
        {
            if ((INT_PTR)ShellExecuteW(NULL, L"open", ABOUT_URL, NULL, NULL, SW_SHOWNORMAL) <= 32)
                LogWin32Error(L"ShellExecuteW(about)", GetLastError());
        }
        else if (LOWORD(wParam) == ID_MENU_EXIT)
            PostMessageW(hwnd, WM_APP_EXIT, 0, 0);
        return 0;

    case WM_APP_EXIT:
        s_shutdownRequested = TRUE;
        DestroyWindow(hwnd);
        return 0;

    /* Alt+F4 while our tray menu has focus must not terminate the utility. */
    case WM_CLOSE:
        if (!s_shutdownRequested) return 0;
        break;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam)
        {
            AppendLogLine(L"Windows session ending.");
            s_shutdownRequested = TRUE;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_POWERBROADCAST:
        CancelEscHold(hwnd);
        KillTimer(hwnd, TIMER_CAPS);
        s_capsPending = FALSE;
        if (wParam == PBT_APMRESUMEAUTOMATIC)
            StartKeyboardHook();
        return TRUE;

    /* ── Win+` hotkey ── */
    case WM_HOTKEY:
        if (wParam == HOTKEY_WIN_BACKTICK) SwitchToNextAppWindow();
        return 0;

    case WM_CAPS_DOWN:
    {
        DWORD elapsed;
        s_capsPending = TRUE;
        s_capsLongFired = FALSE;
        s_capsPressedAt = (DWORD)wParam;
        elapsed = (DWORD)GetTickCount64() - s_capsPressedAt;
        if (!SetTimer(hwnd, TIMER_CAPS,
                       elapsed < CAPS_HOLD_MS ? CAPS_HOLD_MS - elapsed : 1, NULL))
            LogWin32Error(L"SetTimer(CapsLock)", GetLastError());
        return 0;
    }

    case WM_CAPS_UP:
        KillTimer(hwnd, TIMER_CAPS);
        if (s_capsPending && !s_capsLongFired)
        {
            if ((DWORD)((DWORD)wParam - s_capsPressedAt) >= CAPS_HOLD_MS)
                ToggleCapsLock();
            else
                SendShortcut(VK_SPACE, VK_LCONTROL, VK_RCONTROL, L"SendInput(Ctrl+Space)");
        }
        s_capsPending = FALSE;
        return 0;

    case WM_ESC_DOWN:
        CancelEscHold(hwnd);
        s_escPressedAt = (DWORD)wParam;
        s_escTarget = (HWND)lParam;
        s_escTracking = TRUE;
        if (!SetTimer(hwnd, TIMER_ESC_DELAY, ESC_DELAY_MS, NULL))
        {
            LogWin32Error(L"SetTimer(ESC delay)", GetLastError());
            CancelEscHold(hwnd);
        }
        return 0;

    case WM_ESC_UP:
        CancelEscHold(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_CAPS)
        {
            if (s_capsPending && !s_capsLongFired &&
                InterlockedCompareExchange(&s_capsDown, 0, 0) &&
                (DWORD)GetTickCount64() - s_capsPressedAt >= CAPS_HOLD_MS)
            {
                s_capsLongFired = TRUE;
                ToggleCapsLock();
            }
            KillTimer(hwnd, TIMER_CAPS);
        }
        else if (wParam == TIMER_ESC_DELAY)
        {
            KillTimer(hwnd, TIMER_ESC_DELAY);
            TickEscHold(hwnd);
            if (s_escTracking)
            {
                ShowEscOverlay();
                if (!SetTimer(hwnd, TIMER_ESC_TICK, ESC_TICK_MS, NULL))
                {
                    LogWin32Error(L"SetTimer(ESC tick)", GetLastError());
                    CancelEscHold(hwnd);
                }
            }
        }
        else if (wParam == TIMER_ESC_TICK)
            TickEscHold(hwnd);
        else if (wParam == TIMER_HOTKEY_RETRY)
            RegisterWinBacktickHotkey(hwnd);
        else if (wParam == TIMER_TRAY_RETRY)
            AddTrayIcon(hwnd);
        else if (wParam == TIMER_HEALTH)
        {
            DWORD error = (DWORD)InterlockedExchange(&s_hookPostError, 0);
            if (error) LogWin32Error(L"PostMessageW(keyboard)", error);
            if (s_hKeyboardThread &&
                WaitForSingleObject(s_hKeyboardThread, 0) == WAIT_OBJECT_0)
            {
                AppendLogLine(L"Keyboard thread exited unexpectedly; reinstalling hook.");
                CancelEscHold(hwnd);
                KillTimer(hwnd, TIMER_CAPS);
                s_capsPending = FALSE;
                StartKeyboardHook();
            }
        }
        return 0;

    case WM_DESTROY:
        s_shutdownRequested = TRUE;
        UnregisterHotKey(hwnd, HOTKEY_WIN_BACKTICK);
        KillTimer(hwnd, TIMER_CAPS);
        KillTimer(hwnd, TIMER_ESC_DELAY);
        KillTimer(hwnd, TIMER_ESC_TICK);
        KillTimer(hwnd, TIMER_HOTKEY_RETRY);
        KillTimer(hwnd, TIMER_TRAY_RETRY);
        KillTimer(hwnd, TIMER_HEALTH);
        HideEscOverlay();
        if (s_trayIconAdded)
        {
            Shell_NotifyIconW(NIM_DELETE, &s_nid);
            s_trayIconAdded = FALSE;
        }
        PostQuitMessage(0);
        return 0;

    default:
        /* Explorer 重启后会广播 TaskbarCreated，需要重新添加托盘图标 */
        if (msg == s_wmTaskbarCreated && s_wmTaskbarCreated != 0)
        {
            s_trayIconAdded = FALSE;
            AddTrayIcon(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


/* ════════════════════════════════════════════════════════════════
 *  Entry point
 * ════════════════════════════════════════════════════════════════ */
_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    MSG         msg;
    int         messageResult;

    (void)hPrev; (void)lpCmd; (void)nShow;
    s_hInst = hInst;
    SetUnhandledExceptionFilter(CrashFilter);
    AppendLogLine(L"MacKeyFlow starting.");

    if (!EnsureElevated()) return 1;
    if (!AcquireSingleInstance()) return 1;
    MigrateLegacyStartup();

    /* Hidden top-level window: message-only windows miss shell broadcasts. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS_MAIN;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogWin32Error(L"RegisterClassExW(main)", GetLastError());
        MessageBoxW(NULL, L"无法创建程序窗口，程序不会启动。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    s_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 WND_CLASS_MAIN, PRODUCT_NAME, WS_POPUP,
                                 0, 0, 0, 0,
                                 NULL, NULL, hInst, NULL);
    if (!s_hwndMain)
    {
        LogWin32Error(L"CreateWindowExW(main)", GetLastError());
        MessageBoxW(NULL, L"无法创建程序窗口，程序不会启动。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    /* ── Overlay window class ── */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS_OVERLAY;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogWin32Error(L"RegisterClassExW(overlay)", GetLastError());
        DestroyWindow(s_hwndMain);
        return 1;
    }

    /* ── Register TaskbarCreated message (Explorer restart recovery) ── */
    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (s_wmTaskbarCreated)
        ChangeWindowMessageFilterEx(s_hwndMain, s_wmTaskbarCreated, MSGFLT_ALLOW, NULL);
    if (!s_wmTaskbarCreated)
        LogWin32Error(L"RegisterWindowMessageW(TaskbarCreated)", GetLastError());

    /* ── Tray icon ── */
    HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!hIcon)
        hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    if (!hIcon)
        LogWin32Error(L"LoadIconW", GetLastError());

    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize           = sizeof(s_nid);
    s_nid.hWnd             = s_hwndMain;
    s_nid.uID              = 1;
    s_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    s_nid.uCallbackMessage = WM_TRAY_MSG;
    s_nid.hIcon            = hIcon;
    lstrcpyW(s_nid.szTip, PRODUCT_NAME);

    AddTrayIcon(s_hwndMain);

    /* ── Keyboard hook (CapsRemap + EscClose) ── */
    if (!StartKeyboardHook())
    {
        MessageBoxW(NULL, L"键盘钩子安装失败，CapsLock 和 ESC 功能不可用。",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
    }

    /* ── Win+` hotkey (WinBacktick: cycle same-app windows) ── */
    RegisterWinBacktickHotkey(s_hwndMain);
    SetTimer(s_hwndMain, TIMER_HEALTH, HOTKEY_RETRY_MS, NULL);

    for (;;)
    {
        messageResult = GetMessageW(&msg, NULL, 0, 0);
        if (messageResult > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (messageResult < 0)
            LogWin32Error(L"GetMessageW", GetLastError());
        break;
    }

    StopKeyboardHook();
    if (IsWindow(s_hwndMain)) DestroyWindow(s_hwndMain);
    AppendLogLine(L"MacKeyFlow stopped.");
    if (s_hMutex)
    {
        CloseHandle(s_hMutex);
        s_hMutex = NULL;
    }
    return 0;
}
