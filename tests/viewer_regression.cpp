// Local integration test. Controls only the newly launched viewer process.
// F6 is injected only after an owned test window is confirmed foreground.
#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <iomanip>

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << std::endl;
}

std::wstring title(HWND window) {
    wchar_t buffer[1024]{};
    GetWindowTextW(window, buffer, 1024);
    return buffer;
}

bool wait_title(HWND window, const wchar_t* expected) {
    for (int i = 0; i < 50; ++i) {
        if (title(window).find(expected) != std::wstring::npos) return true;
        Sleep(50);
    }
    return false;
}

void send(HWND window, UINT message, WPARAM value, LPARAM data = 0) {
    DWORD_PTR result{};
    require(SendMessageTimeoutW(window, message, value, data,
        SMTO_ABORTIFHUNG, 2000, &result) != 0, "UI message responded within 2s");
}

struct TestSession {
    PROCESS_INFORMATION process{};
    HWND window{};
    HWND focus_window{};
    POINT original_cursor{};
    TestSession() { GetCursorPos(&original_cursor); }
    ~TestSession() {
        if (window && IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 90);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
        if (focus_window) DestroyWindow(focus_window);
        SetCursorPos(original_cursor.x, original_cursor.y);
    }
};

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        require(argc == 3, "arguments: viewer.exe absolute-log-path OR --defaults");
        require(!FindWindowW(L"LGWLViewerWindow", nullptr), "no existing viewer session");
        std::vector<RECT> targets;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO info{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(monitor, &info) && !(info.dwFlags & MONITORINFOF_PRIMARY))
                reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcMonitor);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&targets));
        require(targets.size() == 1, "exactly one non-primary monitor");
        const RECT target = targets.front();
        const LONG targetWidth = target.right - target.left, targetHeight = target.bottom - target.top;
        require(targetWidth * 9 == targetHeight * 16, "target uses a 16:9 preset");
        TestSession session;
        std::wstring command = L"\"" + std::wstring(argv[1]) +
            L"\" --monitor-match \"VDD by MTT\" --require-refresh-hz 60 --auto-exit-ms 20000 --log \"" + argv[2] + L"\"";
        if (std::wstring(argv[2]) == L"--defaults") {
            command = L"\"" + std::wstring(argv[1]) + L"\"";
        }
        // Closing this test viewer must not rearrange unrelated user windows.
        command += L" --rescue-process-id " + std::to_wstring(GetCurrentProcessId());
        command += L" --hotkey-config \"" + std::wstring(argv[1]) + L".test-" + std::to_wstring(GetCurrentProcessId()) + L".conf\"";
        STARTUPINFOW start{sizeof(STARTUPINFOW)};
        start.dwFlags = STARTF_USESHOWWINDOW;
        start.wShowWindow = SW_SHOWNOACTIVATE;
        require(CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &start, &session.process) != FALSE, "launch owned viewer process");
        struct Search { DWORD pid; HWND window; } search{session.process.dwProcessId, nullptr};
        for (int i = 0; i < 100 && !search.window; ++i) {
            EnumWindows([](HWND window, LPARAM data) -> BOOL {
                auto* search = reinterpret_cast<Search*>(data);
                DWORD pid{};
                GetWindowThreadProcessId(window, &pid);
                wchar_t name[64]{};
                GetClassNameW(window, name, 64);
                if (pid == search->pid && std::wstring(name) == L"LGWLViewerWindow") {
                    search->window = window;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
            Sleep(50);
        }
        session.window = search.window;
        std::wostringstream startupScale;
        startupScale << std::fixed << std::setprecision(1) << 100.0 * std::min(
            std::lround(targetWidth * 2.0 / 3.0) / static_cast<double>(targetWidth),
            std::lround(targetHeight * 2.0 / 3.0) / static_cast<double>(targetHeight)) << L"%";
        require(session.window && wait_title(session.window, startupScale.str().c_str()), "startup indicates the actual rounded two-thirds scale");
        Sleep(500); // Allow the initial capture frame to initialize.
        DWORD affinity{};
        require(GetWindowDisplayAffinity(session.window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
            "viewer excluded from capture");
        for (const auto commandAndScale : {std::pair<UINT, double>{1001,1.0}, {1003,0.5}, {1002,2.0/3.0}}) {
            send(session.window, WM_COMMAND, commandAndScale.first);
            RECT client{};
            GetClientRect(session.window, &client);
            require(client.right == std::lround(targetWidth * commandAndScale.second) && client.bottom == std::lround(targetHeight * commandAndScale.second),
                "size menu produces exact requested client pixels");
        }
        SetWindowPos(session.window, nullptr, target.left + 100, target.top + 100, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT actual{}, overlap{};
        GetWindowRect(session.window, &actual);
        require(!IntersectRect(&overlap, &actual, &target), "moving viewer onto capture target is rejected");
        require(wait_title(session.window, L"F6でメイン画面へ"), "global F6 registered and indicated");
        POINT beforeCursor{}, afterCursor{};
        RECT beforeClip{}, afterClip{};
        auto moveIntoViewer = [&](int x, int y) {
            POINT point{x,y};
            ClientToScreen(session.window, &point);
            SetCursorPos(point.x, point.y);
            Sleep(60); // Settle real mouse-leave/move messages before synthetic button messages.
        };
        RECT currentClient{};GetClientRect(session.window,&currentClient);
        const int centerX=currentClient.right/2,centerY=currentClient.bottom/2;
        moveIntoViewer(centerX, centerY);
        GetCursorPos(&beforeCursor);
        GetClipCursor(&beforeClip);
        send(session.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(centerX, centerY));
        GetCursorPos(&afterCursor);
        require(beforeCursor.x == afterCursor.x && beforeCursor.y == afterCursor.y,
            "mouse-down does not warp while the button is held");
        send(session.window, WM_LBUTTONUP, 0, MAKELPARAM(centerX, centerY));
        GetCursorPos(&afterCursor);
        require(std::abs(afterCursor.x - (target.left + targetWidth/2)) <= 1 && std::abs(afterCursor.y - (target.top + targetHeight/2)) <= 1,
            "release maps 66.7 percent center to virtual monitor center");
        GetClipCursor(&afterClip);
        require(EqualRect(&beforeClip, &afterClip), "click jump does not change cursor confinement");
        RECT window{}, client{};
        GetWindowRect(session.window, &window);
        GetClientRect(session.window, &client);
        require(window.right - window.left == client.right && window.bottom - window.top == client.bottom,
            "borderless client occupies the complete window");
        HWND chrome = FindWindowW(L"SharedVDHoverChrome", nullptr);
        HWND ring = FindWindowW(L"SharedVDCursorRing", nullptr);
        require(chrome && ring && GetWindow(chrome, GW_OWNER) == session.window,
            "owned hover title and cursor ring exist");
        Sleep(150);
        require(!IsWindowVisible(chrome), "title hides after cursor jumps out of viewer");
        POINT overViewer{client.right / 2, client.bottom / 2};
        ClientToScreen(session.window, &overViewer);
        SetCursorPos(overViewer.x, overViewer.y);
        Sleep(150);
        require(IsWindowVisible(chrome), "title appears while pointer is over viewer");
        RECT clientAfterHover{};
        GetClientRect(session.window, &clientAfterHover);
        require(EqualRect(&client, &clientAfterHover), "hover title does not resize or rescale the image");
        for (const auto sizeAndPoint : {std::pair<UINT, LONG>{1003,targetWidth/8}, {1001,targetWidth/4}}) {
            send(session.window, WM_COMMAND, sizeAndPoint.first);
            GetClientRect(session.window, &client);
            moveIntoViewer(sizeAndPoint.second, client.bottom / 2);
            send(session.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(sizeAndPoint.second, client.bottom / 2));
            send(session.window, WM_LBUTTONUP, 0, MAKELPARAM(sizeAndPoint.second, client.bottom / 2));
            GetCursorPos(&afterCursor);
            require(afterCursor.x == target.left + targetWidth/4 && afterCursor.y == target.top + targetHeight/2,
                "off-center coordinates map correctly at 50 and 100 percent");
        }
        SetWindowPos(session.window, nullptr, 100, 100, 800, 800, SWP_NOZORDER | SWP_NOACTIVATE);
        moveIntoViewer(200, 400);
        send(session.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 400));
        send(session.window, WM_LBUTTONUP, 0, MAKELPARAM(200, 400));
        GetCursorPos(&afterCursor);
        require(afterCursor.x == target.left + targetWidth/4 && afterCursor.y == target.top + targetHeight/2,
            "letterboxed content maps correctly");
        moveIntoViewer(200, 100);
        GetCursorPos(&beforeCursor);
        send(session.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 100));
        send(session.window, WM_LBUTTONUP, 0, MAKELPARAM(200, 100));
        GetCursorPos(&afterCursor);
        require(beforeCursor.x == afterCursor.x && beforeCursor.y == afterCursor.y,
            "black-bar clicks do not jump");
        WNDCLASSW focusClass{};
        focusClass.lpfnWndProc = DefWindowProcW;
        focusClass.hInstance = GetModuleHandleW(nullptr);
        focusClass.lpszClassName = L"VKomadoRegressionFocus";
        RegisterClassW(&focusClass);
        session.focus_window = CreateWindowExW(WS_EX_TOPMOST, focusClass.lpszClassName, L"VKomado F6 test target",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 450, 250, nullptr, nullptr, focusClass.hInstance, nullptr);
        SetForegroundWindow(session.focus_window);
        if (GetForegroundWindow() != session.focus_window) {
            POINT focusPoint{100,100};
            ClientToScreen(session.focus_window, &focusPoint);
            SetCursorPos(focusPoint.x, focusPoint.y);
            require(WindowFromPoint(focusPoint) == session.focus_window, "focus click targets only the owned helper");
            INPUT mouse[2]{};
            mouse[0].type = mouse[1].type = INPUT_MOUSE;
            mouse[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            mouse[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            require(SendInput(2, mouse, sizeof(INPUT)) == 2, "activate owned helper with a real click");
            MSG message{};
            for (int i=0; i<20 && GetForegroundWindow() != session.focus_window; ++i) {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                Sleep(10);
            }
        }
        require(GetForegroundWindow() == session.focus_window, "owned helper has foreground for real F6 test");
        SetCursorPos(target.left + 960, target.top + 540);
        INPUT keys[2]{};
        keys[0].type = keys[1].type = INPUT_KEYBOARD;
        keys[0].ki.wVk = keys[1].ki.wVk = VK_F6;
        keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
        require(SendInput(2, keys, sizeof(INPUT)) == 2, "send F6 while viewer has no focus");
        MONITORINFO primary{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &primary);
        const POINT center{(primary.rcMonitor.left + primary.rcMonitor.right) / 2,
                           (primary.rcMonitor.top + primary.rcMonitor.bottom) / 2};
        for (int i=0; i<30 && !IsWindowVisible(ring); ++i) Sleep(10);
        GetCursorPos(&afterCursor);
        std::cout << "F6 observed=" << afterCursor.x << "," << afterCursor.y << " expected=" << center.x << "," << center.y << std::endl;
        require(afterCursor.x == center.x && afterCursor.y == center.y,
            "real global F6 returns cursor to primary monitor center");
        require(IsWindowVisible(ring), "F6 shows temporary cursor locator ring");
        require(GetForegroundWindow() == session.focus_window, "return and ring do not steal keyboard focus");
        require((GetWindowLongPtrW(ring, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) ==
            (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT), "ring does not activate or intercept pointer input");
        Sleep(800);
        require(!IsWindowVisible(ring), "ring automatically disappears");
        GetClipCursor(&afterClip);
        require(EqualRect(&beforeClip, &afterClip), "F6 also leaves confinement unchanged");
        send(session.window, WM_COMMAND, 1007);
        HWND editor = FindWindowW(L"SharedVDHotkeyEditor", nullptr);
        require(editor != nullptr, "viewer opens shortcut editor");
        SendDlgItemMessageW(editor,2100,HKM_SETHOTKEY,MAKEWORD('K',HOTKEYF_CONTROL|HOTKEYF_ALT),0);
        SendDlgItemMessageW(editor,2101,HKM_SETHOTKEY,MAKEWORD('J',HOTKEYF_CONTROL|HOTKEYF_ALT),0);
        send(editor,WM_COMMAND,IDOK);
        require(!IsWindow(editor) && wait_title(session.window,L"Ctrl+Alt+Kでメイン画面へ"),"saving updates active viewer shortcut and title");
        require(RegisterHotKey(nullptr,900,MOD_NOREPEAT,VK_F6)!=FALSE,"old F6 is released after customization");
        UnregisterHotKey(nullptr,900);
        SetForegroundWindow(session.focus_window);
        if(GetForegroundWindow()!=session.focus_window) {
            POINT point{100,100};ClientToScreen(session.focus_window,&point);SetCursorPos(point.x,point.y);
            require(WindowFromPoint(point)==session.focus_window,"custom shortcut focus click remains on owned fixture");
            INPUT mouse[2]{};mouse[0].type=mouse[1].type=INPUT_MOUSE;mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,mouse,sizeof(INPUT));
            for(int i=0;i<30&&GetForegroundWindow()!=session.focus_window;++i){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}
        }
        require(GetForegroundWindow()==session.focus_window,"owned helper receives custom shortcut test input");
        auto customKey=[&](WORD key){INPUT input[6]{};for(auto& item:input)item.type=INPUT_KEYBOARD;
            input[0].ki.wVk=VK_CONTROL;input[1].ki.wVk=VK_MENU;input[2].ki.wVk=key;
            input[3].ki.wVk=key;input[4].ki.wVk=VK_MENU;input[5].ki.wVk=VK_CONTROL;
            for(int i=3;i<6;++i)input[i].ki.dwFlags=KEYEVENTF_KEYUP;
            require(SendInput(6,input,sizeof(INPUT))==6,"send configured global shortcut");};
        SetCursorPos(target.left+targetWidth/2,target.top+targetHeight/2);customKey('K');
        for(int i=0;i<50;++i){GetCursorPos(&afterCursor);if(afterCursor.x==center.x&&afterCursor.y==center.y)break;Sleep(10);}
        require(afterCursor.x==center.x&&afterCursor.y==center.y,"custom Ctrl+Alt+K returns cursor globally");
        customKey('J');HWND chooser=nullptr;
        for(int i=0;i<50&&!chooser;++i){chooser=FindWindowW(L"SharedVDWindowPicker",nullptr);Sleep(10);}
        require(chooser!=nullptr,"custom Ctrl+Alt+J opens window picker globally");
        send(chooser,WM_KEYDOWN,VK_ESCAPE);
        send(session.window,WM_COMMAND,1007);editor=FindWindowW(L"SharedVDHotkeyEditor",nullptr);
        require(editor!=nullptr,"shortcut editor can be reopened");
        send(editor,WM_COMMAND,2121);send(editor,WM_COMMAND,IDOK);
        require(wait_title(session.window,L"F6でメイン画面へ"),"default shortcuts can be restored live");
        DestroyWindow(session.focus_window);
        session.focus_window = nullptr;
        for (int i=0; i<20; ++i) {
            SetWindowPos(session.window, nullptr, 100, 100, 1000 + i*15, 650 + i*5,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(20);
        }
        send(session.window, WM_COMMAND, 1001);
        require(wait_title(session.window, L"100.0%"), "resizing remains responsive and exact size can be restored");
        GetWindowRect(session.window, &window);
        const auto hit = SendMessageW(session.window, WM_NCHITTEST, 0, MAKELPARAM(window.left + 1, window.top + 1));
        require(hit == HTTOPLEFT, "borderless corner supports native resize");
        auto chromeClick = [&](int button) {
            RECT chromeRect{};
            GetClientRect(chrome, &chromeRect);
            const int buttonWidth = MulDiv(48, GetDpiForWindow(session.window), 96);
            const int x = chromeRect.right - (4-button) * buttonWidth + buttonWidth/2;
            send(chrome, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, chromeRect.bottom/2));
            send(chrome, WM_LBUTTONUP, 0, MAKELPARAM(x, chromeRect.bottom/2));
        };
        chromeClick(2);
        require(IsZoomed(session.window), "hover title maximize button works");
        GetWindowRect(session.window, &window);
        std::cout << "max_rect=" << window.left << "," << window.top << "," << window.right << "," << window.bottom
                  << " work_rect=" << primary.rcWork.left << "," << primary.rcWork.top << ","
                  << primary.rcWork.right << "," << primary.rcWork.bottom << std::endl;
        require(EqualRect(&window, &primary.rcWork), "maximized borderless window respects primary work area");
        send(session.window, WM_KEYDOWN, VK_F11);
        GetWindowRect(session.window, &window);
        require(EqualRect(&window, &primary.rcMonitor), "F11 from maximized fills the monitor including taskbar area");
        send(session.window, WM_KEYDOWN, VK_ESCAPE);
        GetWindowRect(session.window, &window);
        require(EqualRect(&window, &primary.rcWork), "Escape restores previous maximized work area");
        chromeClick(2);
        require(!IsZoomed(session.window), "hover title restore button works");
        chromeClick(1);
        require(IsIconic(session.window), "hover title minimize button works");
        ShowWindow(session.window, SW_RESTORE);
        chromeClick(3);
        require(WaitForSingleObject(session.process.hProcess, 5000) == WAIT_OBJECT_0, "viewer exits normally");
        DWORD exitCode{};
        GetExitCodeProcess(session.process.hProcess, &exitCode);
        require(exitCode == 0, "viewer exit code is zero");
        require(!IsWindow(chrome) && !IsWindow(ring), "overlays are destroyed on exit");
        require(RegisterHotKey(nullptr, 60, MOD_NOREPEAT, VK_F6) != FALSE, "F6 is released on viewer exit");
        {
            TestSession conflict;
            require(CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE,
                0, nullptr, nullptr, &start, &conflict.process) != FALSE, "launch viewer with F6 already reserved");
            for (int i=0; i<100 && !conflict.window; ++i) {
                HWND candidate = FindWindowW(L"LGWLViewerWindow", nullptr);
                DWORD pid{};
                if (candidate) GetWindowThreadProcessId(candidate, &pid);
                if (pid == conflict.process.dwProcessId) conflict.window = candidate;
                Sleep(50);
            }
            require(conflict.window && wait_title(conflict.window, L"F6登録不可"),
                "hotkey conflict is visible instead of claiming a working F6");
            send(conflict.window, WM_COMMAND, 1004);
            GetCursorPos(&afterCursor);
            require(afterCursor.x == center.x && afterCursor.y == center.y,
                "return menu remains usable when global F6 is unavailable");
            PostMessageW(conflict.window, WM_CLOSE, 0, 0);
            require(WaitForSingleObject(conflict.process.hProcess, 5000) == WAIT_OBJECT_0,
                "conflict-mode viewer exits normally");
        }
        UnregisterHotKey(nullptr, 60);
        std::cout << "regression_result=PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
