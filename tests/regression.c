/* Regression checks use real Win32 timers and Task Scheduler XML parsing, but
 * capture injected keystrokes so no user's application receives test input. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

static DWORD testTime = 10000;
static HWND testForeground = (HWND)(UINT_PTR)0x1234;
static BOOL testControlHeld;
static INPUT capturedInputs[32];
static UINT capturedCount;
static UINT capturedCalls;
static int assertions;
static int failures;

static UINT WINAPI CaptureInput(UINT count, LPINPUT inputs, int size)
{
    (void)size;
    if (count <= 32) memcpy(capturedInputs, inputs, count * sizeof(INPUT));
    capturedCount = count;
    ++capturedCalls;
    return count;
}

static SHORT WINAPI ReadTestKey(int key)
{
    return testControlHeld && key == VK_LCONTROL ? -32768 : 0;
}

static ULONGLONG WINAPI ReadTestTime(void) { return testTime; }
static HWND WINAPI ReadTestForeground(void) { return testForeground; }
static BOOL WINAPI IsTestWindow(HWND hwnd)
{
    return hwnd == (HWND)(UINT_PTR)0x1234 || IsWindow(hwnd);
}
static DWORD WINAPI TestWindowThread(HWND hwnd, LPDWORD pid)
{
    if (hwnd == (HWND)(UINT_PTR)0x1234)
    {
        if (pid) *pid = GetCurrentProcessId() + 1;
        return GetCurrentThreadId();
    }
    return GetWindowThreadProcessId(hwnd, pid);
}

#define KEYFLOW_TESTING
#define SendInput CaptureInput
#define GetAsyncKeyState ReadTestKey
#define GetTickCount64 ReadTestTime
#define GetForegroundWindow ReadTestForeground
#define IsWindow IsTestWindow
#define GetWindowThreadProcessId TestWindowThread
#include "../macos-keyflow.c"

static void Check(BOOL success, const char* description)
{
    ++assertions;
    if (!success) { ++failures; printf("FAIL: %s\n", description); }
}

static void ResetCapture(void)
{
    capturedCount = capturedCalls = 0;
    ZeroMemory(capturedInputs, sizeof(capturedInputs));
    testControlHeld = FALSE;
}

static void Key(UINT vk, UINT message, DWORD time, DWORD flags)
{
    KBDLLHOOKSTRUCT event;
    ZeroMemory(&event, sizeof(event));
    event.vkCode = vk;
    event.time = time;
    event.flags = flags;
    OnKeyboardEvent(HC_ACTION, message, (LPARAM)&event);
}

static void DrainMessages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) continue;
        DispatchMessageW(&msg);
    }
}

static void CapsChecks(HWND hwnd)
{
    ResetCapture();
    testTime = 10000;
    WndProc(hwnd, WM_CAPS_DOWN, 10000, 0);
    WndProc(hwnd, WM_CAPS_UP, 10100, 0);
    Check(capturedCount == 4 && capturedInputs[1].ki.wVk == VK_SPACE,
          "CapsLock tap sends Ctrl+Space");
    WndProc(hwnd, WM_TIMER, TIMER_CAPS, 0);
    Check(capturedCalls == 1, "queued stale timer cannot toggle CapsLock after a tap");

    ResetCapture();
    s_capsDown = TRUE;
    testTime = 20000;
    WndProc(hwnd, WM_CAPS_DOWN, 20000, 0);
    testTime = 20400;
    WndProc(hwnd, WM_TIMER, TIMER_CAPS, 0);
    Check(capturedCount == 2 && capturedInputs[0].ki.wVk == VK_CAPITAL,
          "CapsLock hold toggles CapsLock");
    WndProc(hwnd, WM_CAPS_UP, 20500, 0);
    Check(capturedCalls == 1, "long hold release never sends a second action");
    s_capsDown = FALSE;

    ResetCapture();
    testTime = 32000;
    WndProc(hwnd, WM_CAPS_DOWN, 30000, 0);
    WndProc(hwnd, WM_CAPS_UP, 30500, 0);
    Check(capturedCount == 2 && capturedInputs[0].ki.wVk == VK_CAPITAL,
          "delayed UI processing preserves physical hold duration");

    ResetCapture();
    testControlHeld = TRUE;
    WndProc(hwnd, WM_CAPS_DOWN, 32000, 0);
    WndProc(hwnd, WM_CAPS_UP, 32100, 0);
    Check(capturedCount == 2 && capturedInputs[0].ki.wVk == VK_SPACE &&
          capturedInputs[1].ki.wVk == VK_SPACE,
          "tap preserves a Control key already held by the user");

    ResetCapture();
    testTime = 0xFFFFFF00u;
    WndProc(hwnd, WM_CAPS_DOWN, testTime, 0);
    WndProc(hwnd, WM_CAPS_UP, 0xB0u, 0);
    Check(capturedCount == 2 && capturedInputs[0].ki.wVk == VK_CAPITAL,
          "hold duration survives GetTickCount wraparound");
}

static void EscChecks(HWND hwnd)
{
    ResetCapture();
    testTime = 40000;
    testForeground = (HWND)(UINT_PTR)0x1234;
    s_escDown = TRUE;
    WndProc(hwnd, WM_ESC_DOWN, testTime, (LPARAM)testForeground);
    testTime = 40999;
    WndProc(hwnd, WM_TIMER, TIMER_ESC_TICK, 0);
    Check(capturedCalls == 0, "Esc cannot close before one second");
    testTime = 41000;
    WndProc(hwnd, WM_TIMER, TIMER_ESC_TICK, 0);
    Check(capturedCount == 4 && capturedInputs[1].ki.wVk == VK_F4,
          "Esc closes at one second including the overlay delay");
    Check(s_escDown, "Esc firing preserves physical held state");
    Key(VK_ESCAPE, WM_KEYDOWN, 41010, 0);
    DrainMessages();
    WndProc(hwnd, WM_TIMER, TIMER_ESC_TICK, 0);
    Check(!s_escTracking && capturedCalls == 1,
          "Esc auto-repeat cannot close a second window");
    Key(VK_ESCAPE, WM_KEYUP, 41100, 0);
    DrainMessages();

    ResetCapture();
    testTime = 50000;
    s_escDown = TRUE;
    WndProc(hwnd, WM_ESC_DOWN, testTime, (LPARAM)testForeground);
    testForeground = (HWND)(UINT_PTR)0x5678;
    testTime = 51000;
    WndProc(hwnd, WM_TIMER, TIMER_ESC_TICK, 0);
    Check(capturedCalls == 0 && !s_escTracking,
          "changing foreground window cancels Esc close");

    ResetCapture();
    testForeground = (HWND)(UINT_PTR)0x1234;
    WndProc(hwnd, WM_ESC_DOWN, testTime, (LPARAM)testForeground);
    WndProc(hwnd, WM_ESC_UP, testTime + 100, 0);
    testTime += 1000;
    WndProc(hwnd, WM_TIMER, TIMER_ESC_TICK, 0);
    Check(capturedCalls == 0, "releasing Esc cancels close");
    s_escDown = FALSE;
    Key(VK_ESCAPE, WM_KEYDOWN, testTime, LLKHF_INJECTED);
    Check(!s_escDown, "injected Escape is never treated as physical input");
}

static void SchedulerChecks(void)
{
    Scheduler scheduler;
    ITaskDefinition* definition = NULL;
    ITaskSettings* settings = NULL;
    IActionCollection* actions = NULL;
    IAction* action = NULL;
    IExecAction* exec = NULL;
    BSTR duration = NULL;
    BSTR command = NULL;
    BSTR xmlString = NULL;
    VARIANT_BOOL flag = VARIANT_TRUE;
    WCHAR xml[4096];
    WCHAR tiny[3];
    HWND windows[] = { (HWND)(UINT_PTR)30, (HWND)(UINT_PTR)10, (HWND)(UINT_PTR)20 };
    LPCWSTR path = L"C:\\A & B\\<test>\\macos-keyflow.exe";
    HRESULT hr;

    qsort(windows, _countof(windows), sizeof(HWND), CompareWindowHandles);
    Check((UINT_PTR)windows[0] == 10 && (UINT_PTR)windows[1] == 20 &&
          (UINT_PTR)windows[2] == 30, "three-window cycle keeps order independent of Z order");

    Check(!EscapeXml(L"&", tiny, _countof(tiny)), "XML escaping rejects undersized buffers");
    Check(BuildStartupXml(path, L"S-1-5-21-1-2-3-1001", xml, _countof(xml)),
          "startup XML fits the bounded buffer");
    hr = OpenScheduler(&scheduler);
    Check(SUCCEEDED(hr), "connect to Windows Task Scheduler without elevation");
    if (FAILED(hr)) return;
    hr = ITaskService_NewTask(scheduler.service, 0, &definition);
    xmlString = SysAllocString(xml);
    if (SUCCEEDED(hr)) hr = ITaskDefinition_put_XmlText(definition, xmlString);
    Check(SUCCEEDED(hr), "Windows accepts generated startup task schema");
    if (FAILED(hr)) goto cleanup;
    hr = ITaskDefinition_get_Settings(definition, &settings);
    if (SUCCEEDED(hr)) hr = ITaskSettings_get_ExecutionTimeLimit(settings, &duration);
    Check(SUCCEEDED(hr) && duration && wcscmp(duration, L"PT0S") == 0,
          "startup has no execution time limit");
    hr = ITaskSettings_get_DisallowStartIfOnBatteries(settings, &flag);
    Check(SUCCEEDED(hr) && flag == VARIANT_FALSE, "battery power allows startup");
    hr = ITaskSettings_get_StopIfGoingOnBatteries(settings, &flag);
    Check(SUCCEEDED(hr) && flag == VARIANT_FALSE, "battery transition never stops the app");
    hr = ITaskDefinition_get_Actions(definition, &actions);
    if (SUCCEEDED(hr)) hr = IActionCollection_get_Item(actions, 1, &action);
    if (SUCCEEDED(hr)) hr = IAction_QueryInterface(action, &IID_IExecAction, (void**)&exec);
    if (SUCCEEDED(hr)) hr = IExecAction_get_Path(exec, &command);
    Check(SUCCEEDED(hr) && command && wcscmp(command, path) == 0,
          "task path survives XML special characters without shell quoting");
cleanup:
    SysFreeString(command);
    SysFreeString(duration);
    SysFreeString(xmlString);
    if (exec) IExecAction_Release(exec);
    if (action) IAction_Release(action);
    if (actions) IActionCollection_Release(actions);
    if (settings) ITaskSettings_Release(settings);
    if (definition) ITaskDefinition_Release(definition);
    CloseScheduler(&scheduler);
}

int main(void)
{
    WNDCLASSW wc;
    DWORD handlesBefore = 0;
    DWORD handlesAfter = 0;
    s_hInst = GetModuleHandleW(NULL);
    ZeroMemory(&wc, sizeof(wc));
    wc.hInstance = s_hInst;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = WND_CLASS_MAIN;
    Check(RegisterClassW(&wc) != 0, "create regression window class");
    s_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW, WND_CLASS_MAIN, L"Regression", WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, s_hInst, NULL);
    Check(s_hwndMain != NULL, "create hidden top-level window");
    if (!s_hwndMain) return 1;
    CapsChecks(s_hwndMain);
    EscChecks(s_hwndMain);
    WndProc(s_hwndMain, WM_CLOSE, 0, 0);
    Check(IsWindow(s_hwndMain), "tray focus Alt+F4 does not close the utility");
    SchedulerChecks();
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    for (int i = 0; i < 20; ++i)
    {
        Check(StartKeyboardHook(), "hook worker starts");
        Check(StopKeyboardHook(), "hook worker stops");
    }
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    Check(handlesAfter <= handlesBefore + 2, "repeated hook restarts do not leak handles");
    DestroyWindow(s_hwndMain);
    printf("%d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
